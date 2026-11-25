import os, socket, struct, subprocess, sys, time
# Fake Lapis app-window daemon for testing lapis-doom under qemu-aarch64-static.
# Usage: FAKE_INPUTS="[(5.0,5)]" python3 tools/fake_daemon.py "$PWD/engine/lapis-doom" path/to/IWAD SECONDS [doom args]
from PIL import Image
H = struct.Struct("<4sHHHHIIIQQQII")
d, c = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
os.set_inheritable(c.fileno(), True)
env = dict(os.environ, LAPIS_FRAME_FD=str(c.fileno()), LAPIS_WINDOW_ID="7", LAPIS_WINDOW_GEN="3")
p = subprocess.Popen(["qemu-aarch64-static", sys.argv[1], "-iwad", sys.argv[2]] + sys.argv[4:], env=env,
                     pass_fds=(c.fileno(),), cwd="/tmp", stdout=open("/tmp/doom.log", "w"), stderr=subprocess.STDOUT)
c.close()
def send(kind, flags=0, vg=1, seq=0, tok=0, arg=0):
    d.send(H.pack(b"LPFW", 1, kind, 56, flags, 7, 3, vg, seq, tok, arg, 0, 0))
send(2, flags=1 | (1 << 8), vg=1, tok=99)
frames = 0; serial = 0; last = None; t_end = time.time() + float(sys.argv[3]); errs = []
send(3, seq=1, tok=99); serial = 1
inputs = eval(os.environ.get("FAKE_INPUTS","[]"))
t0 = time.time()
while time.time() < t_end:
    d.settimeout(0.5)
    try: pkt = d.recv(20000)
    except socket.timeout: continue
    f = H.unpack(pkt[:56])
    if f[0] != b"LPFW" or f[2] != 1: errs.append(("bad", f[:3])); continue
    if len(pkt) != 12056 or f[5] != 7 or f[6] != 3 or f[9] != 99 or not (f[4] & 1): errs.append(("hdr", len(pkt), f)); 
    frames += 1; last = pkt[56:]
    send(4, seq=f[8], tok=99)
    el = time.time() - t0
    while inputs and el >= inputs[0][0]:
        send(8, flags=inputs.pop(0)[1], tok=99)
    time.sleep(0.03)
    serial += 1; send(3, seq=serial, tok=99)
send(6, flags=6); time.sleep(0.5); p.kill()
print("frames", frames, "errors", errs[:3])
if last:
    Image.frombytes("1", (400, 240), last).save(os.environ.get("FAKE_FRAME_OUT", "test-frame.png"))
