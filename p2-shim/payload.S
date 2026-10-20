/* The XNU payload from tools/mkpayload.py; build.sh passes its path. */
	.section .payload, "a"
	.balign	16
	.incbin	PAYLOAD
