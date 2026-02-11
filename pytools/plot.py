import sys
import ctypes
import matplotlib.pyplot as plt
import struct
import time

CTYPE_STRUCT_MAP = {
    "int": "i",      # 4 bytes
    "float": "f",    # 4 bytes
    "double": "d"    # 8 bytes
}

CTYPE_SIZE = {
    "int": 4,
    "float": 4,
    "double": 8
}

def parse_arg(arg):
    try:
        typ, addr, length = arg.split(":")
        addr = int(addr, 16)
        length = int(length)
        if typ not in CTYPE_STRUCT_MAP:
            raise ValueError(f"Type inconnu: {typ}")
        return typ, addr, length
    except Exception as e:
        raise ValueError(f"Argument invalide '{arg}': {e}")

def read_memory(pid, addr, length, typ):
    mem_file = f"/proc/{pid}/mem"
    size = CTYPE_SIZE[typ] * length
    fmt = CTYPE_STRUCT_MAP[typ] * length

    try:
        with open(mem_file, "rb") as f:
            f.seek(addr)
            raw = f.read(size)
    except PermissionError:
        print("Erreur: Permission refusée. Lance le script en root ou avec sudo.")
        sys.exit(1)
    except Exception as e:
        print(f"Erreur lecture mémoire: {e}")
        sys.exit(1)

    return list(struct.unpack(fmt, raw))

def main():
    if len(sys.argv) != 3:
        print("Usage: sudo python plot.py <pid> <type>:<addr>:<len>")
        sys.exit(1)

    pid = int(sys.argv[1])
    typ, addr, length = parse_arg(sys.argv[2])

    plt.ion()  # mode interactif
    fig, ax = plt.subplots()
    line, = ax.plot([], [], marker='o')
    ax.set_title(f"Memoire live: {typ} @ {hex(addr)} (PID {pid})")
    ax.set_xlabel("Index")
    ax.set_ylabel("Valeur")
    ax.grid(True)

    while True:
        data = read_memory(pid, addr, length, typ)
        line.set_xdata(range(len(data)))
        line.set_ydata(data)
        ax.relim()      # recalculer les axes
        ax.autoscale_view()
        fig.canvas.draw()
        fig.canvas.flush_events()
        time.sleep(1)   # mise à jour toutes les secondes

if __name__ == "__main__":
    main()

