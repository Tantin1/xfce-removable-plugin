#!/bin/sh
# diagnostico-usb.sh — cómo clasificaría xfce-removable-plugin cada aparato USB
# (copia en shell de las reglas de classify_usb descritas en CONTEXT.md, parches 18–22).
#
#   sh tools/diagnostico-usb.sh          foto de lo que está enchufado ahora
#   sh tools/diagnostico-usb.sh vigilar  eventos en vivo (usb, input, hidraw) con tiempos

if [ "$1" = "vigilar" ]; then
    echo "Enchufá y desenchufá el aparato. Ctrl+C para terminar."
    echo "Mirá cuánto tarda el evento 'input' después del último evento 'usb'"
    echo "(el plugin escanea 300 ms después del último evento 'usb')."
    echo
    exec udevadm monitor --udev -s usb -s input -s hidraw
fi

prop() { udevadm info -q property -p "$1" 2>/dev/null | grep -q "^$2=1\$"; }

for link in /sys/bus/usb/devices/*; do
    name=${link##*/}
    case $name in *:*) continue ;; esac          # interfaces, no aparatos
    d=$(readlink -f "$link")
    [ -r "$d/bDeviceClass" ] || continue

    dc=$(cat "$d/bDeviceClass")
    rem=$(cat "$d/removable" 2>/dev/null || echo "?")
    vid=$(cat "$d/idVendor"); pid=$(cat "$d/idProduct")
    prod=$(cat "$d/product" 2>/dev/null || echo "(sin nombre)")
    manuf=$(cat "$d/manufacturer" 2>/dev/null)

    img= sec= prn= cam= aud= midi= ser= stor= std= xin= gip= adb=
    joy= tab= kbd= tok= net= bt= tty= mtp=
    ifaces= marcas=

    # Interfaces: clase/subclase/protocolo(driver)
    for i in "$d"/"$name":*; do
        [ -r "$i/bInterfaceClass" ] || continue
        c=$(cat "$i/bInterfaceClass"); s=$(cat "$i/bInterfaceSubClass")
        p=$(cat "$i/bInterfaceProtocol")
        drv=; [ -L "$i/driver" ] && drv=$(basename "$(readlink "$i/driver")")
        ifaces="$ifaces $c/$s/$p(${drv:--})"
        case $c in 06) img=1;; 0b) sec=1;; 07) prn=1;; 0e) cam=1;; 08) stor=1;; esac
        case $c/$s in 01/02) aud=1;; 01/03) midi=1;; 02/02) ser=1;; esac
        case $c/$s/$p in
            ff/5d/01|ff/5d/81) xin=1;;   # mando Xbox 360 / XInput
            ff/47/d0) gip=1;;   # mando Xbox One / GIP
            ff/42/01) adb=1;;   # Android en modo depuración (ADB)
        esac
        case $drv in
            usb-storage|uas) stor=1;;
            cdc_acm|ftdi_sio|ch341|cp210x|pl2303) ser=1;;
        esac
        case $c in 03|ff) ;; *) std=1;; esac
    done

    # Marcas que udev puso en los hijos
    for x in $(find "$d" -type d -name 'input[0-9]*' 2>/dev/null); do
        prop "$x" ID_INPUT_JOYSTICK && joy=1
        prop "$x" ID_INPUT_TABLET   && tab=1
        prop "$x" ID_INPUT_KEYBOARD && kbd=1
        prop "$x" ID_INPUT_MOUSE    && kbd=1
        prop "$x" ID_INPUT_TOUCHPAD && kbd=1
    done
    for x in $(find "$d" -type d -name 'hidraw[0-9]*' 2>/dev/null); do
        prop "$x" ID_SECURITY_TOKEN && tok=1
    done
    [ -n "$(find "$d" -mindepth 2 -type d -name net 2>/dev/null)" ] && net=1
    [ -n "$(find "$d" -mindepth 2 -type d \( -name bluetooth -o -name 'hci[0-9]*' \) 2>/dev/null)" ] && bt=1
    [ -n "$(find "$d" -mindepth 2 -type d -name 'tty*' 2>/dev/null)" ] && tty=1
    prop "$d" ID_MTP_DEVICE && mtp=1
    sane=; udevadm info -q property -p "$d" 2>/dev/null | grep -q "^libsane_matched=yes$" && sane=1

    [ -n "$joy" ] && marcas="$marcas joystick"
    [ -n "$tab" ] && marcas="$marcas tableta"
    [ -n "$kbd" ] && marcas="$marcas teclado/mouse"
    [ -n "$tok" ] && marcas="$marcas security-token"
    [ -n "$net" ] && marcas="$marcas red"
    [ -n "$bt" ]  && marcas="$marcas bluetooth"
    [ -n "$tty" ] && marcas="$marcas tty"
    [ -n "$mtp" ] && marcas="$marcas mtp"
    [ -n "$sane" ] && marcas="$marcas sane"

    # Mismo orden que classify_usb
    if   [ "$dc" = 09 ];               then r="descartado: hub"
    elif [ "$rem" = fixed ];           then r="descartado: puerto interno (removable=fixed)"
    elif [ -n "$img$mtp" ];            then r="Phone / Camera  [lista + notifica]"
    elif [ -n "$joy$xin$gip" ];        then r="Gamepad  [lista + notifica]"
    elif [ -n "$tab" ];                then r="Graphics tablet  [lista + notifica]"
    elif [ -n "$tok$sec" ];            then r="Security key / Smart card  [lista + notifica]"
    elif [ -n "$prn" ];                then r="Printer  [lista + notifica]"
    elif [ -n "$sane" ];               then r="Scanner  [lista + notifica]"
    elif [ -n "$cam" ];                then r="Camera / Webcam  [lista + notifica]"
    elif [ -n "$aud" ];                then r="Audio device  [lista + notifica]"
    elif [ -n "$midi" ];               then r="MIDI device  [lista + notifica]"
    elif [ -n "$adb" ];                then r="Android phone (sin transferencia de archivos)  [lista + notifica]"
    elif [ -n "$net$bt" ];             then r="OCULTO: red o Bluetooth"
    elif [ -n "$tty$ser" ];            then r="Serial device / Board  [lista + notifica]"
    elif [ -n "$kbd" ];                then r="Keyboard / Mouse  [solo con \"Show keyboards and mice\"; por omisión no notifica]"
    elif [ -n "$stor" ];               then r="OCULTO: almacenamiento (va por UDisks2)"
    elif [ -n "$std" ];                then r="USB device  [lista, NO notifica]"
    else                                    r="OCULTO: HID o de fabricante sin nada reconocible"
    fi

    printf '%s:%s  %s %s\n' "$vid" "$pid" "$manuf" "$prod"
    printf '   clase=%s removable=%s\n   interfaces:%s\n' "$dc" "$rem" "${ifaces:- (ninguna)}"
    printf '   marcas udev:%s\n' "${marcas:- (ninguna)}"
    printf '   => %s\n' "$r"
    [ -n "$xin$gip" ] && [ -z "$joy" ] && \
        echo "   !! Firma de mando Xbox sin joystick de udev: ningún driver lo tomó (¿xpad bloqueado?)."
    echo
done
