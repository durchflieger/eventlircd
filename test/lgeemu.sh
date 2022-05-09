#!/bin/sh
ka="00"
while read -N 9 m ; do
	case "$m" in
		KA\ 00\ FF*)
		       echo -n "Axxxx"
		       sleep 0.5
		       echo "OK${ka}xx"
		       ;;
		KA\ 00\ 01*)
		       echo "AxxxxOK01xx"
		       ka="01"
		       ;;
		KA\ 00\ 00*)
		       echo "AxxxxOK00xx"
		       ka="00"
		       ;;
		KL*)
			echo "LxxxxOKFAxx"
			;;
	esac
done
