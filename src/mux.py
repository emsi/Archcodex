# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded, connection-scoped byte streams. No Codex protocol interpretation."""
import asyncio
import heapq
import json
import struct

MAGIC = b"ACXM\x00\x01\x00\x00"
HEADER = struct.Struct("!IBI")
OPEN, READY, DATA, CREDIT, CLOSE, ERROR = range(1, 7)
CHUNK = 16384
WINDOW = 65536
MAX_FRAME = 65536
MAX_STREAMS = 1024
MAX_PENDING = 1024 * 1024
MAX_CONTROL = 256 * 1024
JSON_LIMIT = 8 * 1024 * 1024


class Closed(ConnectionError):
    pass


def encoded(value):
    return json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode()


class Stream:
    def __init__(self, mux, ident):
        self.mux, self.ident = mux, ident
        self.credit = WINDOW
        self.remaining = WINDOW
        self.pending_ack = 0
        self.buffer = bytearray()
        self.changed = asyncio.Event()
        self.closed = False
        self.error = ""
        self.ready = asyncio.get_running_loop().create_future()

    async def read(self, size=CHUNK):
        # Credit is returned on the next read, after the previous chunk has
        # passed through the consumer's write/drain. Never block the mux reader.
        if self.pending_ack and not self.closed:
            count, self.pending_ack = self.pending_ack, 0
            self.remaining += count
            self.mux.control(self.ident, CREDIT, struct.pack("!I", count))
        while not self.buffer and not self.closed:
            self.changed.clear()
            await self.changed.wait()
        if self.buffer:
            count = min(size, CHUNK, len(self.buffer))
            block = bytes(self.buffer[:count])
            del self.buffer[:count]
            self.pending_ack += count
            return block
        if self.error:
            raise Closed(self.error)
        return b""

    async def write(self, data):
        offset = 0
        while offset < len(data):
            while not self.credit and not self.closed:
                self.changed.clear()
                await self.changed.wait()
            if self.closed or self.mux.closed:
                raise Closed(self.error or "Stream closed")
            count = min(CHUNK, self.credit, len(data) - offset)
            self.credit -= count
            await self.mux.data(self, bytes(data[offset:offset + count]))
            offset += count

    def finish(self, error=""):
        if self.closed:
            return
        self.closed, self.error = True, error
        self.changed.set()
        self.mux.space.set()
        if not self.ready.done():
            self.ready.set_result(error or "Connection closed during stream setup")

    def close(self, error=""):
        if not self.closed:
            # CLOSE must follow previously enqueued data for this stream.
            self.mux.control(self.ident, ERROR if error else CLOSE,
                             error.encode(errors="replace")[:1024], ordered=True)
        self.finish(error)
        self.mux.streams.pop(self.ident, None)


class Mux:
    def __init__(self, reader, writer, accept=None, initiator=True):
        self.reader, self.writer, self.accept = reader, writer, accept
        self.initiator = initiator
        self.next_id = 1 if initiator else 2
        self.peer_last_id = 0
        self.streams, self.handlers = {}, set()
        self.queue = []
        self.sequence = 0
        self.queued_data = self.queued_control = 0
        self.write_changed = asyncio.Event()
        self.space = asyncio.Event()
        self.space.set()
        self.closed = False
        self.done = asyncio.Event()
        self.error = ""
        self.reader_task = self.writer_task = None

    async def start(self):
        self.writer.write(MAGIC)
        await asyncio.wait_for(self.writer.drain(), 10)
        if await asyncio.wait_for(self.reader.readexactly(len(MAGIC)), 10) != MAGIC:
            raise ValueError("Incompatible Archcodex transport protocol")
        self.reader_task = asyncio.create_task(self.receive())
        self.writer_task = asyncio.create_task(self.send())
        return self

    def enqueue(self, ident, kind, payload, priority):
        self.sequence += 1
        heapq.heappush(self.queue, (priority, self.sequence, ident, kind, payload))
        self.write_changed.set()

    def control(self, ident, kind, payload=b"", ordered=False):
        if self.closed:
            return
        if len(payload) > MAX_FRAME or self.queued_control + len(payload) + HEADER.size > MAX_CONTROL:
            self.abort("Transport control queue exceeded its limit")
            return
        self.queued_control += len(payload) + HEADER.size
        self.enqueue(ident, kind, payload, 1 if ordered else 0)

    async def data(self, stream, payload):
        while self.queued_data + len(payload) > MAX_PENDING and not self.closed and not stream.closed:
            self.space.clear()
            await self.space.wait()
        if self.closed or stream.closed:
            raise Closed("Transport closed")
        self.queued_data += len(payload)
        self.enqueue(stream.ident, DATA, payload, 1)
        # Bound one producer's work even when all writes fit in available credit.
        await asyncio.sleep(0)

    async def open(self, description):
        if self.closed:
            raise Closed(self.error or "Transport disconnected")
        if len(self.streams) >= MAX_STREAMS or self.next_id >= 2**32:
            raise Closed("Transport stream limit reached")
        payload = encoded(description)
        if len(payload) > MAX_FRAME:
            raise ValueError("Endpoint description too large")
        stream = Stream(self, self.next_id)
        self.next_id += 2
        self.streams[stream.ident] = stream
        self.control(stream.ident, OPEN, payload)
        try:
            error = await asyncio.wait_for(asyncio.shield(stream.ready), 20)
            if error:
                raise Closed(error)
            return stream
        except BaseException:
            stream.close("Stream setup cancelled")
            raise

    async def incoming(self, stream, description):
        try:
            if self.accept is None:
                raise ValueError("Incoming streams are not supported")
            # Acceptor validates and opens its target before sending READY.
            await self.accept(stream, description)
        except asyncio.CancelledError:
            raise
        except Exception as error:
            stream.close(str(error)[:1024])
        finally:
            stream.close()

    def accepted(self, stream):
        if stream.closed:
            raise Closed("Client disconnected during stream setup")
        stream.ready.set_result("")
        self.control(stream.ident, READY)

    async def receive(self):
        try:
            while True:
                ident, kind, length = HEADER.unpack(await self.reader.readexactly(HEADER.size))
                if not ident or kind not in range(OPEN, ERROR + 1) or length > MAX_FRAME:
                    raise ValueError("Invalid transport frame")
                if (kind == DATA and not 0 < length <= CHUNK) or (kind == CREDIT and length != 4):
                    raise ValueError("Invalid transport data/credit length")
                if kind in (READY, CLOSE) and length:
                    raise ValueError("Unexpected transport control payload")
                payload = await self.reader.readexactly(length)
                stream = self.streams.get(ident)
                if kind == OPEN:
                    if ident % 2 == self.next_id % 2 or ident <= self.peer_last_id:
                        raise ValueError("Invalid or reused transport stream ID")
                    self.peer_last_id = ident
                    if len(self.streams) >= MAX_STREAMS or len(self.handlers) >= MAX_STREAMS:
                        self.control(ident, ERROR, b"Transport stream limit reached")
                        continue
                    description = json.loads(payload)
                    if not isinstance(description, dict):
                        raise ValueError("Invalid endpoint description")
                    stream = self.streams[ident] = Stream(self, ident)
                    task = asyncio.create_task(self.incoming(stream, description))
                    self.handlers.add(task)
                    task.add_done_callback(self.handlers.discard)
                elif stream is None:
                    # In-flight data/credits may follow a local reset. They are
                    # bounded frames and can only refer to previously allocated IDs.
                    limit = self.next_id - 2 if ident % 2 == self.next_id % 2 else self.peer_last_id
                    if ident > limit:
                        raise ValueError("Unknown transport stream")
                elif kind == READY:
                    if stream.ready.done() or ident % 2 != self.next_id % 2:
                        raise ValueError("Unexpected stream acknowledgement")
                    stream.ready.set_result("")
                elif kind == DATA:
                    if not stream.ready.done() or length > stream.remaining:
                        raise ValueError("Transport receive window exceeded")
                    stream.remaining -= length
                    stream.buffer.extend(payload)
                    stream.changed.set()
                elif kind == CREDIT:
                    credit, = struct.unpack("!I", payload)
                    if not credit or stream.credit + credit > WINDOW:
                        raise ValueError("Invalid transport credit")
                    stream.credit += credit
                    stream.changed.set()
                elif kind in (CLOSE, ERROR):
                    stream.finish(payload.decode(errors="replace") if kind == ERROR else "")
                    self.streams.pop(ident, None)
                    self.space.set()
        except asyncio.CancelledError:
            raise
        except (Exception, asyncio.IncompleteReadError) as error:
            self.abort(str(error) or "Transport ended")

    async def send(self):
        try:
            while not self.closed:
                if not self.queue:
                    self.write_changed.clear()
                    await self.write_changed.wait()
                    continue
                _, _, ident, kind, payload = heapq.heappop(self.queue)
                self.writer.write(HEADER.pack(ident, kind, len(payload)) + payload)
                await self.writer.drain()
                if kind == DATA:
                    self.queued_data -= len(payload)
                    self.space.set()
                else:
                    self.queued_control -= len(payload) + HEADER.size
        except asyncio.CancelledError:
            raise
        except Exception as error:
            self.abort(str(error))

    def abort(self, reason="Transport closed"):
        if self.closed:
            return
        self.closed, self.error = True, reason[:1024]
        for stream in self.streams.values():
            stream.finish(self.error)
        self.streams.clear()
        self.space.set()
        self.write_changed.set()
        self.writer.close()
        self.done.set()

    async def stop(self):
        self.abort()
        tasks = [t for t in [self.reader_task, self.writer_task, *self.handlers]
                 if t and t is not asyncio.current_task()]
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        self.queue.clear()


async def relay(left, right):
    """Both endpoints implement async read/write; socket endpoints are adapted."""
    async def copy(source, target):
        while block := await source.read():
            await target.write(block)
    tasks = [asyncio.create_task(copy(left, right)), asyncio.create_task(copy(right, left))]
    try:
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        left.close()
        right.close()
        await asyncio.gather(*(endpoint.wait_closed() for endpoint in (left, right)
                               if isinstance(endpoint, Socket)))


async def produce(stream, operation):
    """Run a one-way inventory producer only while its subscriber is attached."""
    async def closed():
        if await stream.read():
            raise ValueError("Unexpected data on inventory subscription")
    tasks = [asyncio.create_task(operation), asyncio.create_task(closed())]
    try:
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


class Socket:
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer

    async def read(self):
        return await self.reader.read(CHUNK)

    async def write(self, data):
        self.writer.write(data)
        await self.writer.drain()

    def close(self):
        self.writer.close()

    async def wait_closed(self):
        # Closing a transport can otherwise retain a full socket indefinitely
        # when its reader disappears or stops draining.
        try:
            await asyncio.wait_for(self.writer.wait_closed(), 0.5)
        except (ConnectionError, asyncio.TimeoutError, NotImplementedError):
            self.writer.transport.abort()


async def messages(stream):
    buffer = bytearray()
    while block := await stream.read():
        buffer.extend(block)
        if len(buffer) > JSON_LIMIT:
            raise ValueError("Inventory exceeded its limit")
        while b"\n" in buffer:
            line, _, rest = buffer.partition(b"\n")
            buffer[:] = rest
            message = json.loads(line)
            if not isinstance(message, dict):
                raise ValueError("Invalid inventory message")
            yield message
    if buffer:
        raise ValueError("Incomplete inventory message")


async def report(stream, value):
    data = encoded(value)
    if len(data) > JSON_LIMIT:
        raise ValueError("Inventory exceeded its limit")
    await stream.write(data + b"\n")
