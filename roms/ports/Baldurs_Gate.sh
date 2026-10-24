#!/bin/sh

# 1. Path resolution
PORT_DIR="/storage/roms/ports/gemrb"
[ ! -d "$PORT_DIR" ] && PORT_DIR="/sdcard/roms/ports/gemrb"
[ ! -d "$PORT_DIR" ] && PORT_DIR="/mnt/sdcard/roms/ports/gemrb"

LOG="$PORT_DIR/baldurs_gate.log"
echo "=== Starting Baldur's Gate: The Original Saga at $(date) ===" > $LOG
echo "Kernel: $(uname -a 2>/dev/null)" >> $LOG
echo "[PATH CHECK] PORT_DIR=$PORT_DIR" >> $LOG
cd "$PORT_DIR" || { echo "[ERROR] Failed to cd to $PORT_DIR" >> $LOG; exit 1; }
echo "[PATH CHECK] Current working directory: $(pwd)" >> $LOG

# Ensure symlink to /storage/roms/ports/gemrb for config consistency
if [ "$PORT_DIR" != "/storage/roms/ports/gemrb" ] && [ ! -d "/storage/roms/ports/gemrb" ]; then
    mkdir -p /storage/roms/ports 2>/dev/null
    ln -sf "$PORT_DIR" /storage/roms/ports/gemrb 2>/dev/null
    echo "[PATH CHECK] Created symlink $PORT_DIR -> /storage/roms/ports/gemrb" >> $LOG
fi

# Ensure internal directory alias symlinks exist
ln -sf . "$PORT_DIR/GUIScripts/GUIScripts" 2>/dev/null
ln -sf . "$PORT_DIR/unhardcoded/unhardcoded" 2>/dev/null
ln -sf . "$PORT_DIR/games/bg1/data/Data" 2>/dev/null
ln -sf data "$PORT_DIR/games/bg1/Data" 2>/dev/null

# 2. Game Assets Verification
GAME_DATA="$PORT_DIR/games/bg1"
echo "[ASSET CHECK] Checking game data in $GAME_DATA:" >> $LOG
[ -f "$GAME_DATA/chitin.key" ] && echo "  - chitin.key: OK ($(wc -c < "$GAME_DATA/chitin.key") bytes)" >> $LOG || echo "  - chitin.key: MISSING!" >> $LOG
[ -f "$GAME_DATA/dialog.tlk" ] && echo "  - dialog.tlk: OK ($(wc -c < "$GAME_DATA/dialog.tlk") bytes)" >> $LOG || echo "  - dialog.tlk: MISSING!" >> $LOG
[ -d "$GAME_DATA/data" ] && echo "  - data directory: OK ($(ls "$GAME_DATA/data" | wc -l) files)" >> $LOG || echo "  - data directory: MISSING!" >> $LOG
[ -d "$GAME_DATA/override" ] && echo "  - override directory: OK ($(ls "$GAME_DATA/override" | wc -l) files)" >> $LOG || echo "  - override directory: MISSING!" >> $LOG

# 3. Hardware Devices Verification
echo "[HARDWARE CHECK] Checking hardware device nodes:" >> $LOG
chmod 666 /dev/fb* /dev/graphics/fb* /dev/mali* /dev/ump /dev/dri/* /dev/input/* /dev/tty* /dev/uinput 2>/dev/null
echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null
[ -e /dev/fb0 ] && echo "  - Framebuffer /dev/fb0: OK (virtual size: $(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null))" >> $LOG || echo "  - /dev/fb0: NOT FOUND" >> $LOG
[ -e /dev/mali0 ] && echo "  - Mali GPU node /dev/mali0: OK" >> $LOG || { [ -e /dev/mali ] && echo "  - Mali GPU node /dev/mali: OK" >> $LOG || echo "  - Mali GPU node: NOT FOUND" >> $LOG; }
[ -e /dev/mali ] && [ ! -e /dev/mali0 ] && ln -sf /dev/mali /dev/mali0 2>/dev/null
[ -e /dev/mali0 ] && [ ! -e /dev/mali ] && ln -sf /dev/mali0 /dev/mali 2>/dev/null
[ -e /dev/dri/card0 ] && echo "  - DRI node /dev/dri/card0: OK" >> $LOG || echo "  - DRI node /dev/dri/card0: NOT FOUND" >> $LOG

# Launch M17 Dedicated BG Virtual Mouse & Keyboard Mapper
killall -9 m17_gemrb_mapper 2>/dev/null
killall -9 m17_diablo_mapper 2>/dev/null
sleep 1

MAPPER_PID=""
if [ -f "$PORT_DIR/m17_gemrb_mapper" ]; then
    chmod +x "$PORT_DIR/m17_gemrb_mapper" 2>/dev/null
    "$PORT_DIR/m17_gemrb_mapper" >> $LOG 2>&1 &
    MAPPER_PID=$!
    echo "  - Started m17_gemrb_mapper (PID $MAPPER_PID)" >> $LOG
    sleep 1
fi

# 4. Libraries Verification
echo "[LIBRARY CHECK] Checking critical libraries:" >> $LOG
[ -f "$PORT_DIR/libs/libMali.so" ] && echo "  - libMali.so: OK" >> $LOG || echo "  - libMali.so: MISSING" >> $LOG
[ -f "$PORT_DIR/libs/libEGL.so" ] && echo "  - libEGL.so: OK" >> $LOG || echo "  - libEGL.so: MISSING" >> $LOG
[ -f "$PORT_DIR/libs/libGLESv2.so" ] && echo "  - libGLESv2.so: OK" >> $LOG || echo "  - libGLESv2.so: MISSING" >> $LOG
[ -f "$PORT_DIR/libs/libSDL2-2.0.so.0" ] && echo "  - libSDL2-2.0.so.0: OK" >> $LOG || echo "  - libSDL2-2.0.so.0: MISSING" >> $LOG
[ -f "$PORT_DIR/libs/libsdl_compat.so" ] && echo "  - libsdl_compat.so: OK" >> $LOG || echo "  - libsdl_compat.so: MISSING" >> $LOG
[ -f "$PORT_DIR/gemrb" ] && echo "  - gemrb binary: OK" >> $LOG || echo "  - gemrb binary: MISSING" >> $LOG

# 5. Runtime Environment Configuration
export PYTHONHOME="$PORT_DIR"
export PYTHONPATH="$PORT_DIR/lib/python3.11:$PORT_DIR/lib/python3.11/lib-dynload:$PORT_DIR/GUIScripts:$PORT_DIR/GUIScripts/GUIScripts:$PORT_DIR/GUIScripts/bg1"
export SDL_AUDIODRIVER=alsa
export AUDIODEV=default
export HOME="$PORT_DIR"
export FRAMEBUFFER=/dev/fb0
export SDL_VIDEODRIVER=mali
export SDL_VIDEO_GL_DRIVER="$PORT_DIR/libs/libGLESv2.so"
export SDL_VIDEO_EGL_DRIVER="$PORT_DIR/libs/libEGL.so"
export SDL_RENDER_SCALE_QUALITY=linear
export SDL_RENDER_LOGICAL_SIZE_MODE=letterbox
export SDL_FRAMEBUFFER_ACCELERATION=1

LIB_PATH="$PORT_DIR/libs:/storage/emuelec/lib32:/emuelec/lib32:/usr/lib:/lib"
PRELOAD_LIB="$PORT_DIR/libs/libsdl_compat.so"

chmod +x "$PORT_DIR/gemrb" "$PORT_DIR/libs/ld-linux-armhf.so.3" "$PORT_DIR/m17_video_diagnostic" 2>/dev/null

echo "[EXECUTION] Launching GemRB via ld-linux-armhf.so.3..." >> $LOG
echo "SDL_VIDEODRIVER=$SDL_VIDEODRIVER GL=$SDL_VIDEO_GL_DRIVER EGL=$SDL_VIDEO_EGL_DRIVER" >> $LOG

if [ -f "$PORT_DIR/libs/ld-linux-armhf.so.3" ]; then
    "$PORT_DIR/libs/ld-linux-armhf.so.3" \
        --library-path "$LIB_PATH" \
        --preload "$PRELOAD_LIB" \
        "$PORT_DIR/gemrb" -c "$PORT_DIR/GemRB.cfg" >> $LOG 2>&1
else
    LD_LIBRARY_PATH="$LIB_PATH" LD_PRELOAD="$PRELOAD_LIB" "$PORT_DIR/gemrb" -c "$PORT_DIR/GemRB.cfg" >> $LOG 2>&1
fi

RET=$?
echo "[EXECUTION] GemRB finished with exit code $RET" >> $LOG

[ -n "$MAPPER_PID" ] && kill -9 $MAPPER_PID 2>/dev/null
killall -9 m17_gemrb_mapper 2>/dev/null

# Mirror log to /sdcard as well
cp -f "$LOG" /sdcard/baldurs_gate.log 2>/dev/null
sync
