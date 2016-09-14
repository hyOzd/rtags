#
# Regular cron jobs for the rtags package
#
0 4	* * *	root	[ -x /usr/bin/rtags_maintenance ] && /usr/bin/rtags_maintenance
