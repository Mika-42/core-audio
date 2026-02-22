import sys
import matplotlib.pyplot as plt
import struct
import time
from collections import deque

CTYPE_STRUCT_MAP = {
    "int": "i",
    "float": "f",
    "double": "d"
}

CTYPE_SIZE = {
    "int": 4,
    "float": 4,
    "double": 8
}

def parse_arg(arg):
    typ, addr, length = arg.split(":")
    return typ, int(addr, 16), int(length)

def read_memory(pid, addr, length, typ):
    mem_file = f"/proc/{pid}/mem"
    size = CTYPE_SIZE[typ] * length
    fmt = CTYPE_STRUCT_MAP[typ] * length

    with open(mem_file, "rb") as f:
        f.seek(addr)
        raw = f.read(size)

    return list(struct.unpack(fmt, raw))

def main():
    if len(sys.argv) != 4:
        print("Execute first : echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope,\nUsage: python plot.py <pid> <type>:<addr>:<len> <window_size>")
        sys.exit(1)

    pid = int(sys.argv[1])
    typ, addr, length = parse_arg(sys.argv[2])
    window_size = int(sys.argv[3])

    history = deque(maxlen=window_size)

    plt.ion()
    fig, ax = plt.subplots()
    line, = ax.plot([], [])

    ax.set_title("Waveform Live")
    ax.set_xlabel("Samples")
    ax.set_ylabel("Amplitude")

    # 🔥 ÉCHELLE FIXE AUDIO
    ax.set_ylim(-1.0, 1.0)
    ax.set_xlim(0, window_size)

    ax.grid(True)

    while True:
        new_data = read_memory(pid, addr, length, typ)
        history.extend(new_data)

        line.set_xdata(range(len(history)))
        line.set_ydata(history)

        fig.canvas.draw()
        fig.canvas.flush_events()

        time.sleep(0.05)  # 20 updates/sec

if __name__ == "__main__":
    main()

