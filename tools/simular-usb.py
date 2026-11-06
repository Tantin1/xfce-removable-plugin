#!/usr/bin/env python3
"""simular-usb.py — enchufa un aparato USB de mentira para probar el plugin.

    sudo python3 tools/simular-usb.py teclado
    sudo python3 tools/simular-usb.py quitar      (limpia si algo quedó colgado)

Aparatos: teclado, mouse, mando, adb, escaner.

Usa el controlador USB virtual del kernel (dummy_hcd) y un "gadget" armado en
configfs: para el sistema es un aparato real conectado a un puerto, con sus
uevents, drivers y marcas de udev. Queda enchufado hasta que se presiona Enter
(o Ctrl+C, o se cierra la terminal); ahí se desenchufa y se borra todo.

  teclado  HID (usbhid → ID_INPUT_KEYBOARD)        → "Keyboard / Mouse" (parche 20)
  mouse    HID (usbhid → ID_INPUT_MOUSE)           → "Keyboard / Mouse" (parche 20)
  mando    interfaz XInput ff/5d/01                → "Gamepad" (18) y, sin xpad, aviso (19)
  adb      interfaz ff/42/01 (Android depuración)  → oculto hasta el parche 22
  escaner  ff/ff/ff con un ID de escáner que SANE reconoce → oculto hasta el 22

teclado y mouse no mandan teclas ni movimientos: solo existen.
"""

import os
import signal
import struct
import subprocess
import sys
import time

CONFIGFS = "/sys/kernel/config"
GADGET = CONFIGFS + "/usb_gadget/xrp_sim"
FFS_NAME = "xrpsim"
FFS_DIR = "/dev/ffs-" + FFS_NAME

# informes HID estándar de arranque (boot protocol)
KEYBOARD_REPORT = bytes.fromhex(
    "05010906a101050719e029e7150025017501950881029501750881039505"
    "7501050819012905910295017503910395067508150025650507190029658100c0")
MOUSE_REPORT = bytes.fromhex(
    "05010902a1010901a100050919012903150025019503750181029501750581"
    "0305010930093109381581257f750895038106c0c0")

# (vid, pid, fabricante, producto, función)
#   función: ("hid", protocolo, largo_del_informe, informe)
#            ("ffs", clase, subclase, protocolo, tipo_endpoint)  2 = bulk, 3 = interrupt
DEVICES = {
    "teclado": (0x1d6b, 0x0104, "Simulador", "Teclado simulado",
                ("hid", 1, 8, KEYBOARD_REPORT)),
    "mouse":   (0x1d6b, 0x0104, "Simulador", "Mouse simulado",
                ("hid", 2, 4, MOUSE_REPORT)),
    "mando":   (0x045e, 0x028e, "Simulador", "Mando XInput simulado",
                ("ffs", 0xff, 0x5d, 0x01, 3)),
    "adb":     (0x18d1, 0x4ee7, "Simulador", "Android en modo depuracion (simulado)",
                ("ffs", 0xff, 0x42, 0x01, 2)),
    # CanoScan LiDE 110: está en las reglas de SANE (backend genesys)
    "escaner": (0x04a9, 0x1909, "Simulador", "Escaner simulado",
                ("ffs", 0xff, 0xff, 0xff, 2)),
}


def say(msg):
    print(msg, flush=True)


def fail(msg):
    say("ERROR: " + msg)
    sys.exit(1)


def write(path, value):
    mode = "wb" if isinstance(value, bytes) else "w"
    with open(path, mode) as f:
        f.write(value)


def sh(*cmd):
    return subprocess.run(cmd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


# ---------- preparar ----------

def prepare():
    if os.geteuid() != 0:
        fail("hace falta sudo:  sudo python3 " + sys.argv[0] + " " + " ".join(sys.argv[1:]))
    for mod in ("libcomposite", "dummy_hcd"):
        if not sh("modprobe", mod):
            fail("no se pudo cargar el módulo " + mod + " del kernel.")
    if not os.path.isdir(CONFIGFS + "/usb_gadget"):
        sh("mount", "-t", "configfs", "none", CONFIGFS)
    if not os.path.isdir(CONFIGFS + "/usb_gadget"):
        fail("no está configfs (" + CONFIGFS + "/usb_gadget).")
    udcs = [u for u in sorted(os.listdir("/sys/class/udc")) if u.startswith("dummy_udc")]
    if not udcs:
        fail("dummy_hcd está cargado pero no aparece su controlador en /sys/class/udc.")
    return udcs[0]


# ---------- descriptores de FunctionFS ----------

def ffs_descriptors(cls, sub, proto, ep_type):
    def iface():
        # bLength, tipo 4, número 0, alternativa 0, 2 endpoints, clase, subclase, protocolo, string 1
        return struct.pack("<BBBBBBBBB", 9, 4, 0, 0, 2, cls, sub, proto, 1)

    def ep(addr, size):
        interval = 4 if ep_type == 3 else 0
        return struct.pack("<BBBBHB", 7, 5, addr, ep_type, size, interval)

    fs_size = 32 if ep_type == 3 else 64
    hs_size = 32 if ep_type == 3 else 512
    fs = iface() + ep(0x81, fs_size) + ep(0x02, fs_size)
    hs = iface() + ep(0x81, hs_size) + ep(0x02, hs_size)
    body = struct.pack("<II", 3, 3) + fs + hs          # 3 descriptores en full y en high speed
    # cabecera v2: magia 3, largo total, flags HAS_FS_DESC | HAS_HS_DESC
    head = struct.pack("<III", 3, 12 + len(body), 1 | 2)
    descs = head + body

    text = b"Simulado\0"
    s_body = struct.pack("<H", 0x0409) + text
    strings = struct.pack("<IIII", 2, 16 + len(s_body), 1, 1) + s_body
    return descs, strings


# ---------- armar y desarmar el gadget ----------

def build(name):
    vid, pid, manuf, product, func = DEVICES[name]
    os.makedirs(GADGET)
    write(GADGET + "/idVendor", "0x%04x" % vid)
    write(GADGET + "/idProduct", "0x%04x" % pid)
    write(GADGET + "/bcdUSB", "0x0200")
    write(GADGET + "/bcdDevice", "0x0100")
    os.makedirs(GADGET + "/strings/0x409")
    write(GADGET + "/strings/0x409/manufacturer", manuf)
    write(GADGET + "/strings/0x409/product", product)
    write(GADGET + "/strings/0x409/serialnumber", "xrp-sim-" + name)
    os.makedirs(GADGET + "/configs/c.1/strings/0x409")
    write(GADGET + "/configs/c.1/strings/0x409/configuration", "Simulado")
    write(GADGET + "/configs/c.1/MaxPower", "100")

    if func[0] == "hid":
        _, protocol, length, report = func
        fdir = GADGET + "/functions/hid.usb0"
        os.makedirs(fdir)
        write(fdir + "/protocol", str(protocol))
        write(fdir + "/subclass", "1")
        write(fdir + "/report_length", str(length))
        write(fdir + "/report_desc", report)
        os.symlink(fdir, GADGET + "/configs/c.1/hid.usb0")
        return None

    _, cls, sub, proto, ep_type = func
    fdir = GADGET + "/functions/ffs." + FFS_NAME
    os.makedirs(fdir)
    os.symlink(fdir, GADGET + "/configs/c.1/ffs." + FFS_NAME)
    os.makedirs(FFS_DIR, exist_ok=True)
    if not sh("mount", "-t", "functionfs", FFS_NAME, FFS_DIR):
        fail("no se pudo montar functionfs (¿falta el módulo usb_f_fs?).")
    descs, strings = ffs_descriptors(cls, sub, proto, ep_type)
    # ep0 tiene que quedar abierto mientras el aparato esté enchufado
    ep0 = os.open(FFS_DIR + "/ep0", os.O_RDWR)
    os.write(ep0, descs)
    os.write(ep0, strings)
    return ep0


def plug(udc):
    write(GADGET + "/UDC", udc)


def teardown(ep0=None):
    """Desenchufa y borra todo lo que haya del simulador. Se puede llamar siempre."""
    if os.path.exists(GADGET + "/UDC"):
        try:
            write(GADGET + "/UDC", "\n")
        except OSError:
            pass
    if ep0 is not None:
        try:
            os.close(ep0)
        except OSError:
            pass
    if os.path.isdir(GADGET):
        cfg = GADGET + "/configs/c.1"
        if os.path.isdir(cfg):
            for entry in os.listdir(cfg):
                if os.path.islink(cfg + "/" + entry):
                    os.unlink(cfg + "/" + entry)
            for d in (cfg + "/strings/0x409", cfg):
                if os.path.isdir(d):
                    os.rmdir(d)
    if os.path.ismount(FFS_DIR):
        sh("umount", FFS_DIR)
    if os.path.isdir(FFS_DIR):
        try:
            os.rmdir(FFS_DIR)
        except OSError:
            pass
    if os.path.isdir(GADGET):
        fns = GADGET + "/functions"
        for entry in os.listdir(fns) if os.path.isdir(fns) else []:
            os.rmdir(fns + "/" + entry)
        if os.path.isdir(GADGET + "/strings/0x409"):
            os.rmdir(GADGET + "/strings/0x409")
        os.rmdir(GADGET)


# ---------- principal ----------

def main():
    if len(sys.argv) != 2 or sys.argv[1] not in list(DEVICES) + ["quitar"]:
        say("Uso:  sudo python3 " + sys.argv[0] + " APARATO")
        say("Aparatos: " + ", ".join(DEVICES) + "   (o 'quitar' para limpiar)")
        sys.exit(1)
    name = sys.argv[1]

    if name == "quitar":
        if os.geteuid() != 0:
            fail("hace falta sudo.")
        teardown()
        sh("modprobe", "-r", "dummy_hcd")
        say("Listo: no queda nada del simulador.")
        return

    udc = prepare()
    if os.path.isdir(GADGET):
        say("Había un aparato simulado de antes: lo quito primero.")
        teardown()
        time.sleep(1)

    # Ctrl+C, cerrar la terminal o kill: desenchufar igual
    def stop(signum, frame):
        raise KeyboardInterrupt
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)

    ep0 = None
    try:
        ep0 = build(name)
        plug(udc)
        say("Enchufado: " + DEVICES[name][3] + "  (%04x:%04x)" % DEVICES[name][:2])
        say("Mirá el panel. Presioná Enter para desenchufarlo.")
        try:
            input()
        except EOFError:
            pass
    except KeyboardInterrupt:
        say("")
    finally:
        teardown(ep0)
        say("Desenchufado y limpio.")


if __name__ == "__main__":
    main()
