"""Minimal text-protocol MySQL client for local, empty-password SQL tests."""
import socket
import struct


def length_encoded(data, offset):
    marker = data[offset]
    if marker < 251:
        return marker, offset + 1
    if marker == 251:
        return None, offset + 1
    size = {252: 2, 253: 3, 254: 8}[marker]
    end = offset + 1 + size
    return int.from_bytes(data[offset + 1:end], "little"), end


class Connection:
    def __init__(self, port=2881):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=60)
        self.sequence = 0
        greeting = self.read_packet()
        end_version = greeting.index(b"\0", 1)
        lower = end_version + 1 + 4 + 8 + 1
        capabilities = int.from_bytes(greeting[lower:lower + 2], "little")
        upper = lower + 2 + 1 + 2
        capabilities |= int.from_bytes(greeting[upper:upper + 2], "little") << 16
        flags = capabilities & (0x200 | 0x8000 | 0x80000 | 0x20000)
        response = struct.pack("<IIB23s", flags, 16 * 1024 * 1024, 45, b"\0" * 23)
        response += b"root\0\0"
        if flags & 0x80000:
            response += b"mysql_native_password\0"
        self.write_packet(response)
        reply = self.read_packet()
        if reply[0] == 0xfe:
            self.write_packet(b"")
            reply = self.read_packet()
        if reply[0] != 0:
            raise RuntimeError("Local empty-password authentication failed: %r" % reply)

    def receive(self, size):
        data = bytearray()
        while len(data) < size:
            part = self.s.recv(size - len(data))
            if not part:
                raise ConnectionError("Database closed the connection")
            data.extend(part)
        return bytes(data)

    def read_packet(self):
        parts = []
        while True:
            header = self.receive(4)
            size = int.from_bytes(header[:3], "little")
            self.sequence = (header[3] + 1) & 255
            parts.append(self.receive(size))
            if size < 0xffffff:
                return b"".join(parts)

    def write_packet(self, payload):
        self.s.sendall(len(payload).to_bytes(3, "little") + bytes([self.sequence]) + payload)
        self.sequence = (self.sequence + 1) & 255

    def query(self, sql, on_columns=None):
        self.sequence = 0
        self.write_packet(b"\x03" + sql.encode("utf8"))
        packet = self.read_packet()
        rows, columns = [], []

        def error(data):
            message = data[9:] if data[3:4] == b"#" else data[3:]
            return {"kind": "ERROR", "code": int.from_bytes(data[1:3], "little"),
                    "message": message.decode("utf8", "replace"), "partial_rows": rows}

        if packet[0] == 0xff:
            return error(packet)
        if packet[0] == 0:
            return {"kind": "OK"}
        count, _ = length_encoded(packet, 0)
        for _ in range(count):
            packet = self.read_packet()
            if packet[0] == 0xff:
                return error(packet)
            offset = 0
            fields = []
            for _ in range(5):
                size, offset = length_encoded(packet, offset)
                fields.append(packet[offset:offset + size].decode("utf8", "replace"))
                offset += size
            columns.append(fields[4])
        packet = self.read_packet()
        if packet[0] == 0xff:
            return error(packet)
        if on_columns is not None:
            on_columns()
        while True:
            packet = self.read_packet()
            if packet[0] == 0xff:
                return error(packet)
            if packet[0] == 0xfe and len(packet) < 9:
                break
            offset, row = 0, []
            for _ in range(count):
                size, offset = length_encoded(packet, offset)
                row.append(None if size is None else packet[offset:offset + size].decode("utf8"))
                if size is not None:
                    offset += size
            rows.append(row)
        return {"kind": "RS", "columns": columns, "rows": rows}
