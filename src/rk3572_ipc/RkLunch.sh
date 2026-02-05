#!/bin/sh

check_linker()
{
        [ ! -L "$2" ] && ln -sf $1 $2
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

	network_init&
	check_linker /userdata   /usr/www/userdata
	check_linker /media/usb0 /usr/www/usb0
	check_linker /mnt/sdcard /usr/www/sdcard

	# if /data/rkipc not exist, cp /usr/share
	rkipc_ini=/userdata/rkipc.ini
	default_rkipc_ini=/tmp/rkipc-factory-config.ini

	result=`cat /proc/rkisp-vir* | grep rkcif | wc -l`
	if [ "$result"x == "1"x ] ;then
		ln -s -f /usr/share/rkipc.ini $default_rkipc_ini
	fi
	if [ "$result"x == "2"x ] ;then
		ln -s -f /usr/share/rkipc-2x.ini $default_rkipc_ini
	fi
	tmp_md5=/tmp/.rkipc-ini.md5sum
	data_md5=/userdata/.rkipc-default.md5sum
	md5sum $default_rkipc_ini > $tmp_md5
	chk_rkipc=`cat $tmp_md5|awk '{print $1}'`
	rm $tmp_md5
	if [ ! -f $data_md5 ];then
		md5sum $default_rkipc_ini > $data_md5
	fi
	grep -w $chk_rkipc $data_md5
	if [ $? -ne 0 ] ;then
		rm -f $rkipc_ini
		echo "$chk_rkipc" > $data_md5
	fi

	if [ ! -f "$default_rkipc_ini" ];then
		echo "Error: not found rkipc.ini !!!"
		exit -1
	fi
	if [ ! -f "$rkipc_ini" ]; then
		cp $default_rkipc_ini $rkipc_ini -f
	fi

	# avoid MIPI screen not displaying when startup
	export rt_vo_disable_vop=0
	if [ -d "/usr/share/iqfiles" ];then
		rkipc -a /usr/share/iqfiles &
	else
		rkipc &
	fi
}

ulimit -c unlimited
echo "/data/core-%p-%e" > /proc/sys/kernel/core_pattern

echo userspace > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
echo userspace > /sys/devices/system/cpu/cpufreq/policy4/scaling_governor
echo userspace > /sys/devices/system/cpu/cpufreq/policy6/scaling_governor

echo 408000 >  /sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed
echo 408000 >  /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq
echo 408000 >  /sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq
echo 396000 >  /sys/devices/system/cpu/cpufreq/policy4/scaling_setspeed
echo 396000 >  /sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq
echo 396000 >  /sys/devices/system/cpu/cpufreq/policy4/scaling_min_freq
echo 408000 >  /sys/devices/system/cpu/cpufreq/policy6/scaling_setspeed
echo 408000 >  /sys/devices/system/cpu/cpufreq/policy6/scaling_max_freq
echo 408000 >  /sys/devices/system/cpu/cpufreq/policy6/scaling_min_freq

cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq
cat /sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq
cat /sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq

echo userspace > /sys/class/devfreq/2d000000.gpu/governor
echo 300000000 > /sys/class/devfreq/2d000000.gpu/userspace/set_freq
echo 300000000 > /sys/class/devfreq/2d000000.gpu/min_freq
echo 300000000 > /sys/class/devfreq/2d000000.gpu/max_freq

#==========litcore0 RM==========
io -4 0x26014040 0x001c0004 #RM=1
io -4 0x26014044 0x001c0004
io -4 0x26014038 0x00020002
sleep 0.2
io -4 0x26014038 0x00020000
#==========litcore1 RM==========
io -4 0x26016040 0x001c0004 #RM=1 #RM=1
io -4 0x26016044 0x001c0004
io -4 0x26016038 0x00020002
sleep 0.2
io -4 0x26016038 0x00020000
#==========cci RM==========
io -4 0x26018054 0x001c0004 #RM=1
io -4 0x26018000 0x80008000
sleep 0.2
io -4 0x26018000 0x80000000
#==========bigcore RM==========
io -4 0x26012040 0x001c0004 #RM=1
io -4 0x26012044 0x001c0004
io -4 0x26012038 0x00020002
sleep 0.2
io -4 0x26012038 0x00020000
#==========gpu RM==========
io -4 0x2601e000 0x001c0008 #RM=2
io -4 0x2601e004 0x001c0008
io -4 0x2601e00c 0x04000400
sleep 0.2
io -4 0x2601e00c 0x04000000

#==================litcore0
io -4 0x26100024 0x1c000000 #ring=0
io -4 0x26100024 0x01ff0022 #len=34-->~2.0G
io -4 0x2610002c 0x18 #gck_cal_cnt=24
io -4 0x26100020 0x00220022 #gck_mode=1;gck_en=1
io -4 0x26100020 0x00010001 #gck_start=1
io -4 0x26100054 #get freq
io -4 0x260d8308 0x80008000 #clk_litcore0_pvtpll_src_sel select clk_litcore0_pvtpll
io -4 0x260d8304 0x00030001 #clk_litcore0_sel select clk_litcore0_pvtpll_src
sleep 0.1
freq=`io -4 0x26100054 | busybox  awk '{print $2}'`
freq=$((16#${freq}))
echo "litcore0: freq=$freq"

#==================litcore1
io -4 0x26108024 0x1c000000 #ring=0
io -4 0x26108024 0x01ff0022 #len=34--->~2.0G
io -4 0x2610802c 0x18 #gck_cal_cnt=24
io -4 0x26108020 0x00220022 #gck_mode=1;gck_en=1
io -4 0x26108020 0x00010001 #gck_start=1
io -4 0x26108054 #get freq
io -4 0x260e0304 0x00800080 #clk_litcore1_pvtpll_src_sel select clk_litcore1_pvtpll
io -4 0x260e0304 0x00030001 #clk_litcore1_sel select clk_litcore1_pvtpll_src
sleep 0.1
freq=`io -4 0x26108054 | busybox  awk '{print $2}'`
freq=$((16#${freq}))
echo "litcore1: freq=$freq"

#==================cci
io -4 0x260f0024 0x1c000000 #ring=0
io -4 0x260f0024 0x01ff0062 #len=98
io -4 0x260f002c 0x18 #gck_cal_cnt=24
io -4 0x260f0020 0x00220022 #gck_mode=1;gck_en=1
io -4 0x260f0020 0x00010001 #gck_start=1
io -4 0x260f0054 #get freq
io -4 0x260e830c 0x40004000 #clk_cci_pvtpll_src_sel select clk_cci_pvtpll
io -4 0x260e830c 0x0f800000 #aclk_cci_root_div=0
io -4 0x260e830c 0x30002000 #aclk_cci_root_sel select clk_cci_pvtpll_src
sleep 0.1
freq=`io -4 0x260f0054 | busybox  awk '{print $2}'`
freq=$((16#${freq}))
echo "cci: freq=$freq"


#==================bigcore
io -4 0x260f8024 0x1c000000 #ring=0
io -4 0x260f8024 0x01ff0019 #len=25---->~2.2G
io -4 0x260f802c 0x18 #gck_cal_cnt=24
io -4 0x260f8020 0x00220022 #gck_mode=1;gck_en=1
io -4 0x260f8020 0x00010001 #gck_start=1
io -4 0x260f8054 #get freq
io -4 0x260d0304 0x40004000 #clk_bigcore_pvtpll_src_sel select clk_bigcore_pvtpll
io -4 0x260d0304 0x01800080 #clk_bigcore_sel select clk_bigcore_pvtpll_src
sleep 0.1
freq=`io -4 0x260f8054 | busybox  awk '{print $2}'`
freq=$((16#${freq}))
echo "bigcore: freq=$freq"

#==================gpu
io -4 0x26110024 0x1c000000 #ring=0
io -4 0x26110024 0x01ff0037 #len=55 --->1.1G
io -4 0x2611002c 0x18 #gck_cal_cnt=24
io -4 0x26110020 0x00220022 #gck_mode=1;gck_en=1
io -4 0x26110020 0x00010001 #gck_start=1
io -4 0x26110054 #get freq
io -4 0x2609062c 0x00800080 #clk_gpu_pvtpll_src_sel select clk_gpu_pvtpll
io -4 0x26090628 0x01000100 #clk_gpu_inner_sel select clk_gpu_pvtpll_src
sleep 0.1
freq=`io -4 0x26110054 | busybox  awk '{print $2}'`
freq=$((16#${freq}))
echo "gpu: freq=$freq"


post_chk &
