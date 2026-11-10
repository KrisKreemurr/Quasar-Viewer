#!/bin/bash

## Here are some configuration options for Linux Client Users.

## - Avoids using any OpenAL audio driver.
#export LL_BAD_OPENAL_DRIVER=x

# Completely prevent gamemode from enabling even if set to true in the settings
# This can be useful if you run Slipstream on a battery-operated device (i.e. laptop)
# export DISABLE_GAMEMODE=1

## Everything below this line is just for advanced troubleshooters.
##-------------------------------------------------------------------

## - For advanced debugging cases, you can run the viewer under the
##   control of another program, such as strace, gdb, or valgrind.  If
##   you're building your own viewer, bear in mind that the executable
##   in the bin directory will be stripped: you should replace it with
##   an unstripped binary before you run.
if [ -n "${AL_GDB}" ]; then
    export LL_WRAPPER='gdb --args'
fi

if [ -n "${AL_VALGRIND}" ]; then
    export LL_WRAPPER='valgrind --smc-check=all --error-limit=no --log-file=secondlife.vg --leak-check=full --suppressions=/usr/lib/valgrind/glibc-2.5.supp --suppressions=secondlife-i686.supp'
fi

if [ -n "${AL_MANGO}" ]; then
    export LL_WRAPPER='mangohud --dlsym'
fi

## CPU placement on multi-CCD CPUs (AMD Ryzen/Threadripper/EPYC): the L3
## cache is split per-CCD and cross-CCD reads pay Infinity-Fabric latency.
## The viewer now places threads itself (setting SlipstreamThreadAffinity,
## default on): the main thread and the GPU driver threads stay on CCD0,
## worker pools (image decode, texture staging, mesh decode, fetch, HTTP)
## run on the other CCDs. Pinning the WHOLE process to CCD0 (the old
## behavior) crammed ~60 threads onto 6 cores and starved the main thread
## under load; it is kept only as an opt-in: SLIPSTREAM_AFFINITY=legacy.
## SLIPSTREAM_NO_AFFINITY=1 disables both. The pre-1.2 names ALCHEMY_AFFINITY /
## ALCHEMY_NO_AFFINITY are still read when the new ones are not set (#126).
SLIPSTREAM_AFFINITY="${SLIPSTREAM_AFFINITY-${ALCHEMY_AFFINITY}}"
SLIPSTREAM_NO_AFFINITY="${SLIPSTREAM_NO_AFFINITY-${ALCHEMY_NO_AFFINITY}}"
if [ -n "${SLIPSTREAM_NO_AFFINITY}" ]; then
    export SLIPSTREAM_NO_AFFINITY   # the viewer's own thread placement reads it too
fi
if [ -z "${SLIPSTREAM_NO_AFFINITY}" ] && [ "${SLIPSTREAM_AFFINITY}" = "legacy" ] && command -v taskset >/dev/null 2>&1 && [ -d /sys/devices/system/cpu/cpu0/cache ]; then
    cpu0_l3=""
    for f in /sys/devices/system/cpu/cpu0/cache/index*/level; do
        [ "$(cat "$f" 2>/dev/null)" = "3" ] || continue
        cpu0_l3=$(cat "$(dirname "$f")/shared_cpu_list" 2>/dev/null)
        break
    done
    if [ -n "$cpu0_l3" ]; then
        l3_count=$(for f in /sys/devices/system/cpu/cpu*/cache/index*/level; do
            if [ "$(cat "$f" 2>/dev/null)" = "3" ]; then
                cat "$(dirname "$f")/shared_cpu_list" 2>/dev/null
            fi
        done | sort -u | wc -l)
        if [ "$l3_count" -gt 1 ]; then
            export LL_WRAPPER="taskset -c ${cpu0_l3} ${LL_WRAPPER}"
            echo "SLIPSTREAM_AFFINITY=legacy: pinning the whole process to CCD0 cores ${cpu0_l3} ($l3_count L3 cache instances)."
        fi
    fi
fi

## For controlling various sanitizer options
#export ASAN_OPTIONS="halt_on_error=0 detect_leaks=1 symbolize=1"
#export UBSAN_OPTIONS="print_stacktrace=1 print_summary=1 halt_on_error=0"

install_dir=$(dirname "$0")
build_data_file="${install_dir}/build_data.json"
if [ -f "${build_data_file}" ]; then
    channel=$(sed -n 's/.*"Channel"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${build_data_file}")
else
    echo "Error: File ${build_data_file} not found." >&2
    channel="Slipstream" # Fail safely if we're unable to determine the channel
fi

## Allow Gnome 3 to properly display window title in app bar. A package can
## preset these (the Flatpak sets its app ID, so the desktop matches the
## window to the app's icon).
export SDL_VIDEO_WAYLAND_WMCLASS="${SDL_VIDEO_WAYLAND_WMCLASS:-$channel}"
export SDL_VIDEO_X11_WMCLASS="${SDL_VIDEO_X11_WMCLASS:-$channel}"

## - Enable threaded mesa GL impl
export mesa_glthread=true

## - Uncomment to enable nvidia threaded GL optimizations - MAY CAUSE INSTABILITY
#export __GL_THREADED_OPTIMIZATIONS=1

## Nothing worth editing below this line.
##-------------------------------------------------------------------

SCRIPTSRC=$(readlink -f "$0" || echo "$0")
RUN_PATH=$(dirname "${SCRIPTSRC}" || echo .)
echo "Running from ${RUN_PATH}"
cd "${RUN_PATH}" || return

# Installed by a distribution package (e.g. the slipstream-bin Arch package,
# #248): the package provides the desktop entry, the secondlife:// handler and
# the system libraries, so skip the per-user registration and the library check.
packaged_by=""
if [ -f .packaged-by ]; then
    packaged_by=$(cat .packaged-by)
fi

if [ -z "${packaged_by}" ]; then
    # Re-register the secondlife:// protocol handler every launch, for now.
    ./etc/register_secondlifeprotocol.sh

    # Re-register the application with the desktop system every launch, for now.
    ./etc/refresh_desktop_app_entry.sh
fi

# SDL can use native Wayland while the bundled CEF still uses X11/XWayland.
# Keep inherited display/authentication metadata. If a desktop launcher lost
# DISPLAY, recover it only from the user manager for this SAME compositor and
# runtime directory, with a real local X socket. Never guess :0 or force CEF's
# optional Ozone/Wayland backend just because SDL uses Wayland.
slipstream_inherit_xwayland_display()
{
    [ -n "${WAYLAND_DISPLAY:-}" ] || return 0
    [[ "${XDG_RUNTIME_DIR:-}" = /* ]] || return 0
    [ -d "$XDG_RUNTIME_DIR" ] && [ -O "$XDG_RUNTIME_DIR" ] || return 0
    local wayland_socket
    case "$WAYLAND_DISPLAY" in
        /*) wayland_socket="$WAYLAND_DISPLAY" ;;
        *) wayland_socket="$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ;;
    esac
    [ -S "$wayland_socket" ] && [ -O "$wayland_socket" ] || return 0

    # A desktop launch normally already has both values. Keep it completely
    # untouched. SSH launches from a live Wayland session can inherit DISPLAY
    # but lose XAUTHORITY, so only inspect the manager/session when one is
    # missing. XDG_SESSION_TYPE is deliberately not consulted: SSH commonly
    # changes it to "tty" while preserving the live Wayland socket.
    local inherited_display="${DISPLAY:-}" inherited_auth="${XAUTHORITY:-}"
    local display_number="" selected_display="" selected_auth=""
    if [[ "$inherited_display" =~ ^:([0-9]{1,5})(\.[0-9]+)?$ ]]; then
        display_number="${BASH_REMATCH[1]}"
        selected_display="$inherited_display"
    elif [ -n "$inherited_display" ]; then
        # Do not rewrite explicit remote / nonlocal displays.
        return 0
    fi

    if [ -n "$inherited_display" ] && [ -n "$inherited_auth" ]; then
        return 0
    fi
    command -v systemctl >/dev/null 2>&1 || return 0
    command -v timeout >/dev/null 2>&1 || return 0
    local session_env entry manager_display="" manager_wayland="" manager_runtime="" manager_auth=""
    # Filter before capture: do not retain/log unrelated user-manager variables.
    # No eval/source: show-environment output is data, including quoted values.
    if ! session_env=$(
        set -o pipefail
        timeout -k 0.1s 1s systemctl --user show-environment 2>/dev/null |
            LC_ALL=C awk '/^(DISPLAY|WAYLAND_DISPLAY|XDG_RUNTIME_DIR|XAUTHORITY)=/ {
                if (length($0) > 4096 || ++n > 4) exit 1; print
            }'
    ); then
        return 0
    fi
    while IFS= read -r entry; do
        case "$entry" in
            DISPLAY=*) manager_display="${entry#DISPLAY=}" ;;
            WAYLAND_DISPLAY=*) manager_wayland="${entry#WAYLAND_DISPLAY=}" ;;
            XDG_RUNTIME_DIR=*) manager_runtime="${entry#XDG_RUNTIME_DIR=}" ;;
            XAUTHORITY=*) manager_auth="${entry#XAUTHORITY=}" ;;
        esac
    done <<< "$session_env"
    if [ "$manager_wayland" != "$WAYLAND_DISPLAY" ] || [ "$manager_runtime" != "$XDG_RUNTIME_DIR" ]; then
        if [ -z "$selected_display" ]; then
        # Without an inherited local DISPLAY, only adopt the display associated
        # with this exact Wayland/runtime session.
            return 0
        fi
        # For an inherited local DISPLAY, the live Wayland socket and matching
        # Xauthority record are enough. Do not trust stale manager DISPLAY/auth
        # values from another concurrently logged-in session.
        manager_display=""
        manager_auth=""
    fi
    if [ -n "$inherited_auth" ] && [ -n "$manager_auth" ] && [ "$inherited_auth" != "$manager_auth" ]; then
        # A manager-authenticated display from another login must not replace
        # an explicit authentication context.
        return 0
    fi
    if [ -z "$selected_display" ]; then
        [[ "$manager_display" =~ ^:([0-9]{1,5})(\.[0-9]+)?$ ]] || return 0
        display_number="${BASH_REMATCH[1]}"
        selected_display="$manager_display"
    elif [ -n "$manager_display" ]; then
        # If the user manager has a display, it must be this same X server.
        [[ "$manager_display" =~ ^:([0-9]{1,5})(\.[0-9]+)?$ ]] || return 0
        [ "${BASH_REMATCH[1]}" = "$display_number" ] || return 0
    fi
    local xwayland_socket="/tmp/.X11-unix/X${display_number}"
    [ -S "$xwayland_socket" ] && [ -O "$xwayland_socket" ] || return 0

    # KDE may create a private /run/user/$UID/xauth_* file and leave
    # XAUTHORITY unset in a later SSH environment. Never choose a file by name
    # alone: require owner-only permissions and an Xauthority record for this
    # exact local display. `nlist` output (which contains the cookie) flows only
    # through awk and is discarded; it is never stored or logged.
    command -v xauth >/dev/null 2>&1 || return 0
    local auth_candidate auth_mode auth_checks=0
    slipstream_try_xauthority()
    {
        auth_candidate="$1"
        [[ "$auth_candidate" = /* ]] && [ -f "$auth_candidate" ] && [ -r "$auth_candidate" ] && [ -O "$auth_candidate" ] || return 1
        auth_mode=$(stat -Lc '%a' -- "$auth_candidate" 2>/dev/null) || return 1
        [[ "$auth_mode" =~ ^[0-7]{3,4}$ ]] || return 1
        (( (8#$auth_mode & 077) == 0 )) || return 1
        if xauth -f "$auth_candidate" nlist ":${display_number}" 2>/dev/null |
            awk 'NF >= 6 { found=1 } END { exit !found }' >/dev/null 2>&1; then
            selected_auth="$auth_candidate"
            return 0
        fi
        return 1
    }
    if [ -n "$inherited_auth" ]; then slipstream_try_xauthority "$inherited_auth"; fi
    if [ -z "$selected_auth" ] && [ -n "$manager_auth" ] && [ "$manager_auth" != "$inherited_auth" ]; then
        slipstream_try_xauthority "$manager_auth"
    fi
    if [ -z "$selected_auth" ]; then
        for auth_candidate in "$XDG_RUNTIME_DIR"/xauth_*; do
            [ -f "$auth_candidate" ] || continue
            (( auth_checks += 1 ))
            slipstream_try_xauthority "$auth_candidate" && break
            [ "$auth_checks" -lt 16 ] || break
        done
    fi
    if [ -z "$selected_auth" ] && [ -n "${HOME:-}" ]; then
        slipstream_try_xauthority "$HOME/.Xauthority"
    fi
    [ -n "$selected_auth" ] || return 0

    # Fill only missing values; a valid explicit XAUTHORITY was returned above.
    if [ -z "$inherited_display" ]; then
        export DISPLAY="$selected_display"
    fi
    if [ -z "$inherited_auth" ]; then
        export XAUTHORITY="$selected_auth"
    fi
}
slipstream_inherit_xwayland_display

# Tell the CEF plugin which backend the wrapper selected without setting SDL's
# standard SDL_VIDEODRIVER in the viewer process. The plugin translates this
# private marker just before Dullahan initializes.
slipstream_select_cef_display_backend()
{
    case "${SDL_VIDEO_DRIVER:-}" in
        x11) export SLIPSTREAM_CEF_OZONE_PLATFORM=x11; return 0 ;;
        wayland) export SLIPSTREAM_CEF_OZONE_PLATFORM=wayland; return 0 ;;
    esac
    [ -n "${SDL_VIDEODRIVER:-}" ] && return 0
    [ -n "${WAYLAND_DISPLAY:-}" ] || return 0
    [[ "${XDG_RUNTIME_DIR:-}" = /* ]] || return 0
    local wayland_socket
    case "$WAYLAND_DISPLAY" in
        /*) wayland_socket="$WAYLAND_DISPLAY" ;;
        *) wayland_socket="$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ;;
    esac
    [ -S "$wayland_socket" ] && [ -O "$wayland_socket" ] || return 0
    export SLIPSTREAM_CEF_OZONE_PLATFORM=wayland
}

## Before we mess with LD_LIBRARY_PATH, save the old one to restore for
##  subprocesses that care.
export SAVED_LD_LIBRARY_PATH="${LD_LIBRARY_PATH}"

# Add our library directory
export LD_LIBRARY_PATH="$PWD/lib:${LD_LIBRARY_PATH}"

## Auto-update (#421, off unless the person enabled it in Preferences): a viewer that
## downloaded and verified an update left a "pending" note; apply it before starting. The
## apply step swaps the install directory, so this launcher (which just moved to
## <dir>.prev) re-runs itself from the new one. Never for package-manager installs.
if [ -z "${packaged_by}" ] && [ -f etc/update_hooks.sh ]; then
    . etc/update_hooks.sh
    if slipstream_update_apply "${RUN_PATH}" "${RUN_PATH}/bin/do-not-directly-run-slipstream-bin"; then
        export SLIPSTREAM_UPDATE_APPLIED=1
        exec "${RUN_PATH}/slipstream" "$@"
    fi
fi

## Window frame on Wayland (#196/#197). GNOME draws no title bar or border for
## native Wayland windows; SDL3 draws them through libdecor + a libdecor plugin.
## lib/libdecor carries a copy (libdecor + the cairo plugin) for systems that
## lack it. It is deliberately not in lib/ (on LD_LIBRARY_PATH above), so a
## system libdecor wins when it has plugins: it may have the GTK plugin and
## match the desktop theme. The viewer makes the final choice, preloads the
## copy it picked and logs it ("Display backend:" lines in Slipstream.log);
## here we only point libdecor at our plugin when the system has no libdecor
## plugin directory, unless LIBDECOR_PLUGIN_DIR is already set.
if [ -z "${LIBDECOR_PLUGIN_DIR}" ] && [ -d "${RUN_PATH}/lib/libdecor/plugins-1" ]; then
    system_libdecor_plugins=""
    for libdir in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib; do
        if [ -e "${libdir}/libdecor-0.so.0" ] && ls "${libdir}"/libdecor/plugins-1/*.so >/dev/null 2>&1; then
            system_libdecor_plugins="${libdir}/libdecor/plugins-1"
            break
        fi
    done
    if [ -z "${system_libdecor_plugins}" ]; then
        export LIBDECOR_PLUGIN_DIR="${RUN_PATH}/lib/libdecor/plugins-1"
    fi
fi

# The web browser and media-on-a-prim (CEF) need system libraries that the
# viewer itself doesn't (nss, atk, cups, gbm, ...). If one is missing, the
# media plugin fails to load and all web content stays blank (#245). Say so
# up front, naming the libraries; README-linux.txt lists the packages.
# Set SLIPSTREAM_SKIP_MEDIA_CHECK=1 to skip; package installs skip it.
if [ -z "${SLIPSTREAM_SKIP_MEDIA_CHECK}" ] && [ -z "${packaged_by}" ] && [ -f lib/libcef.so ] && command -v ldd >/dev/null 2>&1; then
    cef_missing=$(ldd lib/libcef.so bin/llplugin/libmedia_plugin_cef.so 2>/dev/null \
        | awk '/not found/ {print $1}' | sort -u | tr '\n' ' ')
    if [ -n "${cef_missing}" ]; then
        echo "WARNING: web pages and in-world media will not work. Missing system libraries: ${cef_missing}" >&2
        echo "         See README-linux.txt ('Troubleshooting', 'Web and media') for the packages to install." >&2
        if command -v notify-send >/dev/null 2>&1; then
            notify-send "Slipstream: web and media disabled" \
                "Missing system libraries: ${cef_missing}. See README-linux.txt for the packages to install."
        fi
    fi
fi

# Copy "$@" to ARGS string specifically to delete the --skip-gridargs switch.
# The gridargs.dat file is no more, but we still want to avoid breaking
# scripts that invoke this one with --skip-gridargs.
# Note: In sh, we don't have arrays like in Bash. So, we use a string instead to store the arguments.
# --x11 / --wayland pick the display backend for this run (same as
# SDL_VIDEO_DRIVER=x11 / =wayland, overriding the Display backend preference).
ARGS=""
for ARG in "$@"; do
    case "$ARG" in
        --skip-gridargs) ;;
        --x11) export SDL_VIDEO_DRIVER=x11 ;;
        --wayland) export SDL_VIDEO_DRIVER=wayland ;;
        *) ARGS="$ARGS \"$ARG\"" ;;
    esac
done
slipstream_select_cef_display_backend

# Check chrome-sandbox permissions, and try to set them if they are not already
# SANDBOX_BIN=bin/llplugin/chrome-sandbox
# if set-user-id = false || is writable || executable = false || read is false || is owned by effective uid || is owned by effective gid
# OPTOUT_FILE="bin/llplugin/.user_does_not_want_chrome_sandboxing_and_accepts_the_risks"
# if [ ! -u "$SANDBOX_BIN" ] || [ -w "$SANDBOX_BIN" ] || [ ! -x "$SANDBOX_BIN" ] || [ ! -r "$SANDBOX_BIN" ] || [ -O "$SANDBOX_BIN" ] || [ -G "$SANDBOX_BIN" ]; then
#     echo "$SANDBOX_BIN permissions are not set properly to run under sandboxing."
#     if [ ! -f "$OPTOUT_FILE" ]; then
#         SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
#         pkexec "$SCRIPT_DIR/etc/chrome_sandboxing_permissions_setup.sh"
#     fi
# fi

#setup wine voice
# if command -v wine >/dev/null 2>&1; then
#     export WINEDEBUG=-all # disable all debug output for wine
#     export WINEPREFIX="$HOME/.alchemynext/wine"
#     if [ ! -d "$WINEPREFIX" ]; then
#         DISPLAY="" wine hostname
#     fi
# else
#     export VIEWER_DISABLE_WINE=1
#     echo "Please install wine to enable full voice functionality."
# fi

# Check if switcheroo is needed
if [ -d /sys/class/drm/card1 ] && command -v switcherooctl >/dev/null 2>&1 && [ "$(switcherooctl)" = "" ]; then
    notify-send "Automatic GPU selection is not available" "Please enable switcheroo-control.service"
fi

# Run the program.
# Don't quote $LL_WRAPPER because, if empty, it should simply vanish from the
# command line. But DO quote "$ARGS": preserve separate args as
# individually quoted.
# Note: In sh, we don't have arrays like in Bash. So, we use a string instead to store the arguments.
eval "$LL_WRAPPER bin/do-not-directly-run-slipstream-bin $ARGS"
LL_RUN_ERR=$?

# A freshly installed update that died before it reached the login screen is rolled back
# to the previous install (kept as <dir>.prev) and that build is never installed again.
if type slipstream_update_rollback_if_needed >/dev/null 2>&1 \
   && slipstream_update_rollback_if_needed "${RUN_PATH}" "${LL_RUN_ERR}"; then
    export SLIPSTREAM_UPDATE_APPLIED=1
    exec "${RUN_PATH}/slipstream" "$@"
fi

# Handle any resulting errors
if [ $LL_RUN_ERR -ne 0 ]; then
    # generic error running the binary
    echo "*** Bad shutdown ($LL_RUN_ERR). ***"
    if [ "$(uname -m)" = "x86_64" ]; then
        echo
        cat << EOFMARKER
You are running Slipstream Viewer on a x86_64 platform.
EOFMARKER
    fi
fi
