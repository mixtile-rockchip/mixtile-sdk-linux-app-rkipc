#!/bin/sh

rcS()
{
	for i in /oem/usr/etc/init.d/S??* ;do

		# Ignore dangling symlinks (if any).
		[ ! -f "$i" ] && continue

		case "$i" in
			*.sh)
				# Source shell script for speed.
				(
					trap - INT QUIT TSTP
					set start
					. $i
				)
				;;
			*)
				# No sh extension, so fork subprocess.
				$i start
				;;
		esac
	done
}

network_init()
{
	ethaddr1=`ifconfig -a | grep "eth.*HWaddr" | awk '{print $5}'`

	if [ -f /data/ethaddr.txt ]; then
		ethaddr2=`cat /data/ethaddr.txt`
		if [ $ethaddr1 == $ethaddr2 ]; then
			echo "eth HWaddr cfg ok"
		else
			ifconfig eth0 down
			ifconfig eth0 hw ether $ethaddr2
		fi
	else
		echo $ethaddr1 > /data/ethaddr.txt
	fi
	ifconfig eth0 up && udhcpc -i eth0
}

# Define a function to get the interrupt number and bind it to CPU3
bind_interrupt_to_cpu3() {
    local interrupt_line=$1
    local interrupt_number=$(echo $interrupt_line | awk '{print $1}' | sed 's/:$//')
    local cpu_mask=$((8)) # CPU3 bitmask, assuming CPU numbers start from 0

    # Check if the interrupt number is valid
    if [[ -n $interrupt_number ]]; then
        # Check if the interrupt binding file exists
        if [ -f "/proc/irq/$interrupt_number/smp_affinity" ]; then
            # Write the CPU3 bitmask to the interrupt binding file
            echo $cpu_mask > "/proc/irq/$interrupt_number/smp_affinity"
            echo "Interrupt number $interrupt_number has been bound to CPU3"
        else
            echo "Interrupt number $interrupt_number binding file does not exist"
        fi
    else
        echo "Invalid interrupt number: $interrupt_number"
    fi
}

post_chk()
{
	#TODO: ensure /userdata mount done
	cnt=0
	while [ $cnt -lt 30 ];
	do
		cnt=$(( cnt + 1 ))
		if mount | grep -w userdata; then
			break
		fi
		sleep .1
	done

	network_init &
	# if ko exist, install ko first
	default_ko_dir=/ko
	if [ -f "/oem/usr/ko/insmod_ko.sh" ];then
		default_ko_dir=/oem/usr/ko
	fi
	if [ -f "$default_ko_dir/insmod_ko.sh" ];then
		cd $default_ko_dir && sh insmod_ko.sh && cd -
	fi

	rkipc_ini=/userdata/rkipc.ini

	if [ ! -f "$rkipc_ini" ]; then
		cp /oem/usr/share/rkipc.ini /userdata/ -f
	fi

	# avoid MIPI screen not displaying when startup
	export rt_vo_disable_vop=0

	killall irqbalance

	# Get interrupt numbers and bind them to CPU3, Cannot bind ISP interrupt, otherwise imu pts will fluctuate greatly
	interrupt_lines=$(cat /proc/interrupts | grep -E 'rkcifhw|spi|inv_mpu')
	echo "$interrupt_lines" | while read -r line; do
		bind_interrupt_to_cpu3 "$line"
	done
	# Disable CPU3 idle
	echo 1 > /sys/devices/system/cpu/cpu3/cpuidle/state1/disable

	echo 800 > /sys/bus/iio/devices/iio:device1/in_anglvel_sampling_frequency
	echo 800 > /sys/bus/iio/devices/iio:device1/in_accel_sampling_frequency
	echo 0.004785 > /sys/bus/iio/devices/iio:device1/in_accel_scale
	echo 0.001064225 > /sys/bus/iio/devices/iio:device1/in_anglvel_scale
	echo 121 > /sys/bus/iio/devices/iio:device1/in_accel_lpf_bw
	echo 0 > /sys/bus/iio/devices/iio:device1/in_anglvel_lpf_bw

	# swap sdi0 and sdi1
	rk_mpi_amix_test --control "SAI2 Receive PATH0 Source Select" --value "From SDI1"
	rk_mpi_amix_test --control "SAI2 Receive PATH1 Source Select" --value "From SDI0"

	rkipc &
}

rcS

ulimit -c unlimited
echo "/data/core-%p-%e" > /proc/sys/kernel/core_pattern

post_chk &
