## LD_PRELOAD Hook for the WAS-110

**_Note: Using this hook comes with no warranty and I'm not responsible if you bring down your OLT/cause any damage with it!_**

Compile with openwrt's Docker Image

```
buildbot@d1fb672130bb:/home/build/openwrt/package/ifx_hook_clean$ make -j8
mips-openwrt-linux-gcc -march=24kc -mtune=24kc -mabi=32 -fPIC -Wall -O2 -march=24kc -mtune=24kc -mabi=32 -shared -ldl -o fifo_hook.so fifo_hook.c
mips-openwrt-linux-gcc: warning: environment variable 'STAGING_DIR' not defined
fifo_hook.c: In function 'hex_string_to_bytes':
fifo_hook.c:97:18: warning: implicit declaration of function 'isspace' [-Wimplicit-function-declaration]
     while (*p && isspace(*p)) p++;
                  ^~~~~~~
fifo_hook.c:106:13: warning: implicit declaration of function 'isxdigit' [-Wimplicit-function-declaration]
         if (isxdigit(*p)) {
             ^~~~~~~~
mips-openwrt-linux-gcc: warning: environment variable 'STAGING_DIR' not defined
mips-openwrt-linux-gcc: warning: environment variable 'STAGING_DIR' not defined
Built fifo_hook.so successfully
fifo_hook.so: ELF 32-bit MSB shared object, MIPS, MIPS32 rel2 version 1 (SYSV), dynamically linked, with debug_info, not stripped
buildbot@d1fb672130bb:/home/build/openwrt/package/ifx_hook_clean$ make strip
mips-openwrt-linux-strip fifo_hook.so
Stripped fifo_hook.so
buildbot@d1fb672130bb:/home/build/openwrt/package/ifx_hook_clean$ 
```

Once compiled simply start the `omcid` service with LD_PRELOAD hooked

```sh
start_service() {
	local aon_mode
	local stdout
	local stderr

	if [ "$(pon_ploam_emergency_stop_state_get)" = "1" ]; then
		ploam_emerg_stop_state=-e
	fi

	# by default the output of omcid is redirected to /dev/console (option 2)
	# read values from config to allow changes to this
	stdout="$(uci -q get omci.default.stdout)"
	[ -z "$stdout" ] && stdout=2
	stderr="$(uci -q get omci.default.stderr)"
	[ -z "$stderr" ] && stderr=2

	aon_mode="$(uci -q get optic.common.aon_mode)"
	[ -z "$aon_mode" ] && aon_mode=0

	if [ "$aon_mode" -ne 1 ]; then
		procd_open_instance
		procd_set_param env LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/opt/pon/lib/:/opt/intel/usr/lib/" LD_PRELOAD="/tmp/fifo_hook.so"
		procd_set_param command ${OMCID_BIN} --debug_level=2 ${ploam_emerg_stop_state}
		procd_set_param respawn
		procd_set_param stdout $stdout
		procd_set_param stderr $stderr
		procd_close_instance
	fi
}
```

Loading from `/tmp/fifo_hook.so` as an example

Once hooked, omcid will read a replacements config file from `/ptconf/8311/replacements.ini` with a layout like

```ini
# OMCI Message Replacement Rules
# Format: <message_type> <find_hex_pattern> <replace_hex_pattern>
#     OR: <message_type> <sequence_number>
# 
# Notes:
# - Patterns start at offset 8 in the OMCI message (after 8-byte header)
# - For pattern rules: find and replace patterns must be exactly 64 hex chars (32 bytes)
# - For sequence rules: just message type and sequence number (used for ME 0x00ab)
# - Hex values should be continuous without spaces
# - Comments start with # or ;
# - Empty lines are ignored
#
# Pattern Rule Example:
# 14 00f0000080000000....(64 chars) 015b0002c000....(64 chars)
#
# Sequence Rule Example (for ME 0x00ab cycling replacements):
# 9 1
# 9 2
# 9 3
#
# Message Types:
#  4 = CREATE
#  6 = DELETE  
#  8 = SET
#  9 = GET
# 11 = GET_ALL_ALARMS
# 12 = GET_ALL_ALARMS_NEXT
# 13 = MIB_UPLOAD
# 14 = MIB_UPLOAD_NEXT (most common for MIB replacement)
# 15 = MIB_RESET
# 16 = ALARM
# 17 = AVC
# 18 = TEST
# 19 = START_SW_DOWNLOAD
# 20 = DOWNLOAD_SECTION
# 21 = END_SW_DOWNLOAD
# 22 = ACTIVATE_SOFTWARE
# 23 = COMMIT_SOFTWARE
# 24 = SYNC_TIME
# 25 = REBOOT
# 26 = GET_NEXT
# 27 = TEST_RESULT
# 28 = GET_CURRENT_DATA
# 29 = SET_TABLE
```
