/* drivers/hwmon/sc7660.c
 *
 *allwinner platform

 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */


#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/hwmon-sysfs.h>

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/fs.h>
#include <linux/input-polldev.h>
#include <linux/device.h>
#include "../init-input.h"
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/ioctl.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include <linux/hrtimer.h>
#include <linux/ktime.h>
//#include <mach/system.h>
//#include <mach/hardware.h>
//#undef CONFIG_HAS_EARLYSUSPEND
#undef CONFIG_PM

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#if defined(CONFIG_HAS_EARLYSUSPEND) || defined(CONFIG_PM)
#include <linux/pm.h>
#endif


/*
 * Defines
 */
#define assert(expr)\
	if (!(expr)) {\
		printk("Assertion failed! %s,%d,%s,%s\n",\
			__FILE__, __LINE__, __func__, #expr);\
	}

#define sc7660_DRV_NAME	    "sc7660"
#define SENSOR_NAME 		sc7660_DRV_NAME


#define POLL_INTERVAL_MAX	1000
#define POLL_INTERVAL		50
#define INPUT_FUZZ	2
#define INPUT_FLAT	2
#define TIANJU_FILTER

#define sc7660_REG_CTRL	    		0x20
#define sc7660_REG_CTRL_RANGE 		0x23
#define sc7660_REG_DATA 			0x00

#define sc7660_MEASURING_RANGE      0x30
#define sc7660_MEASURING_RANGE_2G   0x0
#define sc7660_MEASURING_RANGE_4G   0x10
#define sc7660_MEASURING_RANGE_8G   0x20
#define sc7660_MEASURING_RANGE_16G  0x30

/* sc7660 control bit */
#define sc7660_CTRL_PWRON		    0x47	/* power on */
#define sc7660_CTRL_PWRDN		    0x00	/* power donw */


#define MODE_CHANGE_DELAY_MS        100



#define CALIBRATION_NUM		        40
#define AXIS_X_Y_RANGE_LIMIT		200
#define AXIS_X_Y_AVG_LIMIT			400
#define AXIS_Z_RANGE		        200
#define AXIS_Z_DFT_G		        1000
#define GOTO_CALI	       	        100
#define FAILTO_CALI		            101

#define SC7660_ENABLE		    1
#define SC7660_XOUT_L			0x28
#define SC7660_XOUT_H			0x29
#define SC7660_YOUT_L			0x2A
#define SC7660_YOUT_H			0x2B
#define SC7660_ZOUT_L			0x2C
#define SC7660_ZOUT_H			0x2D
#define SC7660_MODE			    0x20
#define SC7660_MODE1			0x21
#define SC7660_MODE2			0x22
#define SC7660_MODE3			0x23
#define SC7660_BOOT			    0x24
#define SC7660_STATUS			0x27

#define GSENSOR_REG_CALI
/*
#ifdef GSENSOR_REG_CALI
static int reg[29] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, \
                      0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, \
                      0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A};
static int reg_data[29] = {0};
#endif
*/

static struct device  			*hwmon_dev = NULL;
static struct i2c_client		*sc7660_i2c_client = NULL;
static struct input_polled_dev  *sc7660_idev = NULL;

struct sc7660_data_s {
        struct i2c_client       *client;
        struct input_polled_dev *pollDev;
        struct mutex 			interval_mutex;

		struct delayed_work		dwork;//allen
		int    time_of_cali;
		atomic_t enable;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
#if defined(CONFIG_PM) || defined(CONFIG_HAS_EARLYSUSPEND)
	volatile int suspend_indator;
#endif
	struct hrtimer hr_timer;
	struct work_struct wq_hrtimer;
	ktime_t ktime;

} sc7660_data;

/* Addresses to scan */
static const unsigned short normal_i2c[] = {0x1D,I2C_CLIENT_END};
static __u32 twi_id = 0;
static struct sensor_config_info gsensor_info = {
	.input_type = GSENSOR_TYPE,
};

enum {
	DEBUG_INIT = 1U << 0,
	DEBUG_CONTROL_INFO = 1U << 1,
	DEBUG_DATA_INFO = 1U << 2,
	DEBUG_SUSPEND = 1U << 3,
};
static u32 debug_mask = 0x0;
#define dprintk(level_mask, fmt, arg...)	if (unlikely(debug_mask & level_mask)) \
	printk(KERN_DEBUG fmt , ## arg)

module_param_named(debug_mask, debug_mask, int, S_IRUGO);

#ifdef CONFIG_HAS_EARLYSUSPEND
static void sc7660_early_suspend(struct early_suspend *h);
static void sc7660_late_resume(struct early_suspend *h);
#endif

static struct workqueue_struct *sc7660_workqueue = NULL;

struct SC7660_acc{
	int    x;
	int    y;
	int    z;
} ;

struct Cali_Data {
	//mis p and n
	unsigned char xpmis; //x axis positive mismatch to write
	unsigned char xnmis; //x axis negtive mismatch to write
	unsigned char ypmis;
	unsigned char ynmis;
	unsigned char zpmis;
	unsigned char znmis;
	//off p and n
	unsigned char xpoff; //x axis positive offset to write
	unsigned char xnoff; //x axis negtive offset to write
	unsigned char ypoff;
	unsigned char ynoff;
	unsigned char zpoff;
	unsigned char znoff;
	//mid mis and off
	unsigned char xmmis; //x axis middle mismatch to write
	unsigned char ymmis; //y axis middle mismatch to write
	unsigned char zmmis; //z axis middle mismatch to write
	unsigned char xmoff; //x axis middle offset to write
	unsigned char ymoff; //y axis middle offset to write
	unsigned char zmoff; //z axis middle offset to write
	//output p and n
	signed int xpoutput; //x axis output of positive mismatch
	signed int xnoutput; //x axis output of negtive mismatch
	signed int ypoutput;
	signed int ynoutput;
	signed int zpoutput;
	signed int znoutput;
	//output
	signed int xfoutput; //x axis the best or the temporary output
	signed int yfoutput; //y axis the best or the temporary output
	signed int zfoutput; //z axis the best or the temporary output
	//final and temp flag
	unsigned char xfinalf; //x axis final flag:if 1,calibration finished
	unsigned char yfinalf; //y axis final flag:if 1,calibration finished
	unsigned char zfinalf; //z axis final flag:if 1,calibration finished
	unsigned char xtempf;  //x axis temp flag:if 1,the step calibration finished
	unsigned char ytempf;  //y axis temp flag:if 1,the step calibration finished
	unsigned char ztempf;  //z axis temp flag:if 1,the step calibration finished

	unsigned char xaddmis;	//x axis mismtach register address
	unsigned char yaddmis;	//y axis mismtach register address
	unsigned char zaddmis;	//z axis mismtach register address
	unsigned char xaddoff;	//x axis offset register address
	unsigned char yaddoff;	//y axis offset register address
	unsigned char zaddoff;	//z axis offset register address


	unsigned char (*MisDataSpaceConvert)(unsigned char continuous);	//mismatch space convert function pointer
	unsigned char (*OffDataSpaceConvert)(unsigned char continuous);	//offset space convert function pointer






};


static int sc7660_acc_get_data( int *xyz);
static void sc7660_reset(void);

static char Read_Reg(unsigned char reg)
{
	char ret;
	ret = i2c_smbus_read_byte_data(sc7660_i2c_client, reg);
	return ret;
}
/*
static signed char Read_Output(char reg)
{

	char ret = 0;
	char acc_buf[6];
    int index = 0;

     while(1)
	 {

		msleep(15);
		ret = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_STATUS);

		if( (ret & 0x08) != 0 )
		{
			break;
		}

        index++;

        if(index > 40)
            break;

	}

	acc_buf[0] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_XOUT_L);
	acc_buf[1] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_XOUT_H);

	acc_buf[2] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_YOUT_L);
	acc_buf[3] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_YOUT_H);

	acc_buf[4] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_ZOUT_L);
	acc_buf[5] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_ZOUT_H);

	if(reg == 0x29)
	{
		return acc_buf[1];
	}
	else if(reg == 0x2b)
	{
		return acc_buf[3];
	}
	else if(reg == 0x2d)
	{
		return acc_buf[5];
	}
	else
		return 0;


}
*/
static void Read_Output_3axis(unsigned char *acc_buf)
{
	char buffer[3] = {0};
	int index = 0;
	//int ret   = 0;
	while(1){
			msleep(20);
			*buffer = SC7660_STATUS;
			//ret = sensor_rx_data(sc7660_client, buffer,1);
				buffer[0] = Read_Reg(0x27);
			if( (buffer[0] & 0x08) != 0 ){break;}
      index++;
      if(index > 40)break;
	}
	//6 register data be read out
	acc_buf[0] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_XOUT_L);
	acc_buf[1] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_XOUT_H);

	acc_buf[2] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_YOUT_L);
	acc_buf[3] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_YOUT_H);

	acc_buf[4] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_ZOUT_L);
	acc_buf[5] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_ZOUT_H);
}



static void Write_Input(char addr, char thedata)
{
	int result;
	//result = sensor_write_reg(sc7660_i2c_client, addr, thedata);
	result = i2c_smbus_write_byte_data(sc7660_i2c_client, addr, thedata);
}

static void tilt_3axis_mtp(signed int x,signed int y,signed int z){
	char buffer[6] = {0};
	unsigned char buffer0[6] = {0};
	unsigned char buffer1[6] = {0};
	signed char mtpread[3]={0};
	signed int xoutp,youtp,zoutp;
	signed int xoutpt,youtpt,zoutpt;
	signed char xtilt,ytilt,ztilt;
	xoutp=youtp=zoutp=0;
	xoutpt=youtpt=zoutpt=0;
	xtilt=ytilt=ztilt=0;
	Read_Output_3axis(buffer0);
	Read_Output_3axis(buffer1);

	xoutpt = ((signed int)((buffer1[1]<<8)|buffer1[0]))>>4;
	youtpt = ((signed int)((buffer1[3]<<8)|buffer1[2]))>>4;
	zoutpt = ((signed int)((buffer1[5]<<8)|buffer1[4]))>>4;

	xoutp = xoutpt-x*16;
	youtp = youtpt-y*16;
	zoutp = zoutpt-z*16;



    buffer[0] = Read_Reg(0x10);
	mtpread[0]=(signed char)buffer[0];

    buffer[0] = Read_Reg(0x11);
	mtpread[1]=(signed char)buffer[0];

    buffer[0] = Read_Reg(0x12);
	mtpread[2]=(signed char)buffer[0];




	//calculate the new tilt mtp value
	xtilt=(signed char)(xoutp/8)+ mtpread[0];
	ytilt=(signed char)(youtp/8)+ mtpread[1];
	ztilt=(signed char)(zoutp/8)+ mtpread[2];


	//write the new into mtp
	Write_Input(0x10,xtilt);
	Write_Input(0x11,ytilt);
	Write_Input(0x12,ztilt);


}

//input:two data
//0 for not near zero
//1 for first data
//2 for second data
/*
static char IsNearZero(unsigned int Pos, unsigned int Neg, unsigned char error)
{
	if(abs(Pos) < error)
		return 1;
	if(abs(Neg) < error)
		return 2;
	return 0;
}
*/


/*
static unsigned char forword_MisDataSpaceConvert(unsigned char continuous)
{
	if(continuous >= 128)
		return continuous - 128;
	else
		return 255 - continuous;
}
*/

static unsigned char reverse_MisDataSpaceConvert(unsigned char continuous)
{
	if(continuous >= 128)
		return continuous;
	else
		return 127 - continuous;
}



static unsigned char reverse_OffDataSpaceConvert(unsigned char continuous)
{
	return 127 - continuous;
}



static unsigned char forword_OffDataSpaceConvert(unsigned char continuous)
{
	return continuous;
}





//set finalflag xfinalf-yfinalf-zfinalf upto the relative output
static void check_output_set_finalflag(struct Cali_Data *pcalidata,unsigned char err){

	if(abs(pcalidata->xfoutput) < err){
		//printk("line:%d Xcali finish!Final=%d\n",__LINE__,pcalidata->xfoutput);
		pcalidata->xfinalf=1;
	}
	if(abs(pcalidata->yfoutput) < err){
		//printk("line:%d Xcali finish!Final=%d\n",__LINE__,pcalidata->yfoutput);
		pcalidata->yfinalf=1;
	}
	if(abs(pcalidata->zfoutput) < err){
		//printk("line:%d Xcali finish!Final=%d\n",__LINE__,pcalidata->zfoutput);
		pcalidata->zfinalf=1;
	}

}




static void check_finalflag_set_tempflag(struct Cali_Data *pcalidata){
	if(pcalidata->xfinalf){pcalidata->xtempf=1;}
	if(pcalidata->yfinalf){pcalidata->ytempf=1;}
	if(pcalidata->zfinalf){pcalidata->ztempf=1;}
}


static unsigned char check_flag_is_return(struct Cali_Data *pcalidata){
	if((pcalidata->xfinalf) && (pcalidata->yfinalf) && (pcalidata->zfinalf))
	{

		return 1;
	}
	else return 0;
}



static void updata_midmis_address(struct Cali_Data *pcalidata){
	if(pcalidata->xtempf==0){
		pcalidata->xmmis=(unsigned char)(((unsigned int)(pcalidata->xpmis) + (unsigned int)(pcalidata->xnmis))/2);
		pcalidata->MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(pcalidata->xaddmis, (*(pcalidata->MisDataSpaceConvert))(pcalidata->xmmis));
	}
	if(pcalidata->ytempf==0){
		pcalidata->ymmis=(unsigned char)(((unsigned int)(pcalidata->ypmis) + (unsigned int)(pcalidata->ynmis))/2);
		//pcalidata->MisDataSpaceConvert = forword_MisDataSpaceConvert;
		pcalidata->MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(pcalidata->yaddmis, (*(pcalidata->MisDataSpaceConvert))(pcalidata->ymmis));
	}
	if(pcalidata->ztempf==0){
		pcalidata->zmmis=(unsigned char)(((unsigned int)(pcalidata->zpmis) + (unsigned int)(pcalidata->znmis))/2);
		pcalidata->MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(pcalidata->zaddmis, (*(pcalidata->MisDataSpaceConvert))(pcalidata->zmmis));
	}
}


static void updata_midoff_address(struct Cali_Data *pcalidata){
	if(pcalidata->xtempf==0){
		pcalidata->xmoff=(unsigned char)(((unsigned int)(pcalidata->xpoff) + (unsigned int)(pcalidata->xnoff))/2);
		pcalidata->OffDataSpaceConvert = reverse_OffDataSpaceConvert;
		Write_Input(pcalidata->xaddoff, (*(pcalidata->OffDataSpaceConvert))(pcalidata->xmoff));
	}
	if(pcalidata->ytempf==0){
		pcalidata->ymoff=(unsigned char)(((unsigned int)(pcalidata->ypoff) + (unsigned int)(pcalidata->ynoff))/2);
		//pcalidata->OffDataSpaceConvert = forword_OffDataSpaceConvert;
		pcalidata->OffDataSpaceConvert = reverse_OffDataSpaceConvert;
		Write_Input(pcalidata->yaddoff, (*(pcalidata->OffDataSpaceConvert))(pcalidata->ymoff));
	}
	if(pcalidata->ztempf==0){
		pcalidata->zmoff=(unsigned char)(((unsigned int)(pcalidata->zpoff) + (unsigned int)(pcalidata->znoff))/2);
		pcalidata->OffDataSpaceConvert = forword_OffDataSpaceConvert;
		Write_Input(pcalidata->zaddoff, (*(pcalidata->OffDataSpaceConvert))(pcalidata->zmoff));
	}
}




static void updata_mmis_pnfoutput_set_tempflag(	struct Cali_Data *pcalidata,
											unsigned char *buf,
										  	signed int xrel,
										  	signed int yrel,
										  	signed int zrel){
	//output 2 struct data
	pcalidata->xfoutput=(signed int)((signed char)buf[1])-xrel;
	pcalidata->yfoutput=(signed int)((signed char)buf[3])-yrel;
	pcalidata->zfoutput=(signed int)((signed char)buf[5])-zrel;

	if(abs(pcalidata->xfoutput)<25)pcalidata->xtempf=1;
	if(abs(pcalidata->yfoutput)<25)pcalidata->ytempf=1;
	if(abs(pcalidata->zfoutput)<25)pcalidata->ztempf=1;

	if(pcalidata->xtempf==0)
	{
		if(pcalidata->xfoutput>0){
			pcalidata->xpoutput = pcalidata->xfoutput;
			pcalidata->xpmis = pcalidata->xmmis;
		}
		else{
			pcalidata->xnoutput = pcalidata->xfoutput;
			pcalidata->xnmis = pcalidata->xmmis;
		}
	}

	if(pcalidata->ytempf==0)
	{
		if(pcalidata->yfoutput>0){
			pcalidata->ypoutput = pcalidata->yfoutput;
			pcalidata->ypmis = pcalidata->ymmis;
		}
		else{
			pcalidata->ynoutput = pcalidata->yfoutput;
			pcalidata->ynmis = pcalidata->ymmis;
		}
	}

	if(pcalidata->ztempf==0)
	{
		if(pcalidata->zfoutput>0){
			pcalidata->zpoutput = pcalidata->zfoutput;
			pcalidata->zpmis = pcalidata->zmmis;
		}
		else{
			pcalidata->znoutput = pcalidata->zfoutput;
			pcalidata->znmis = pcalidata->zmmis;
		}
	}
}



static void updata_moff_pnfoutput_set_tempflag(	struct Cali_Data *pcalidata,
											unsigned char *buf,
										  	signed int xrel,
										  	signed int yrel,
										  	signed int zrel){

	pcalidata->xfoutput=(signed int)((signed char)buf[1])-xrel;
	pcalidata->yfoutput=(signed int)((signed char)buf[3])-yrel;
	pcalidata->zfoutput=(signed int)((signed char)buf[5])-zrel;

	if(abs(pcalidata->xfoutput)<3)pcalidata->xtempf=1;
	if(abs(pcalidata->yfoutput)<3)pcalidata->ytempf=1;
	if(abs(pcalidata->zfoutput)<3)pcalidata->ztempf=1;

	if(pcalidata->xtempf==0)
	{
		if(pcalidata->xfoutput>0){
			pcalidata->xpoutput = pcalidata->xfoutput;
			pcalidata->xpoff = pcalidata->xmoff;
		}
		else{
			pcalidata->xnoutput = pcalidata->xfoutput;
			pcalidata->xnoff = pcalidata->xmoff;
		}
	}

	if(pcalidata->ytempf==0)
	{
		if(pcalidata->yfoutput>0){
			pcalidata->ypoutput = pcalidata->yfoutput;
			pcalidata->ypoff = pcalidata->ymoff;
		}
		else{
			pcalidata->ynoutput = pcalidata->yfoutput;
			pcalidata->ynoff = pcalidata->ymoff;
		}
	}

	if(pcalidata->ztempf==0)
	{
		if(pcalidata->zfoutput>0){
			pcalidata->zpoutput = pcalidata->zfoutput;
			pcalidata->zpoff = pcalidata->zmoff;
		}
		else{
			pcalidata->znoutput = pcalidata->zfoutput;
			pcalidata->znoff = pcalidata->zmoff;
		}
	}
}

int auto_calibration_instant(signed int x, signed int y, signed int z){


	unsigned char count=0,cyclecount=0;
	unsigned char acc_buf[6];

	struct Cali_Data calidata={0};


	calidata.xaddmis = 0x41;//0x40; x and y mismatch addr swap
	calidata.yaddmis = 0x40;//0x41;
	calidata.zaddmis = 0x42;
	calidata.xaddoff = 0x47;
	calidata.yaddoff = 0x48;
	calidata.zaddoff = 0x49;
#ifdef PRINT
	printf("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(UINT)calidata.xfinalf,(UINT)calidata.xtempf,
				(UINT)calidata.yfinalf,(UINT)calidata.ytempf,
				(UINT)calidata.zfinalf,(UINT)calidata.ztempf
				);
#endif




	Read_Output_3axis(acc_buf);
	calidata.xfoutput=(signed int)((signed char)acc_buf[1])-x;
	calidata.yfoutput=(signed int)((signed char)acc_buf[3])-y;
	calidata.zfoutput=(signed int)((signed char)acc_buf[5])-z;
	check_output_set_finalflag(&calidata,2);
	if(check_flag_is_return(&calidata)){
		printk("step1:=file=%s,line=%d，calibration ok \n",__FILE__,__LINE__);
		return 1;
	}
#ifdef PRINT
	printf("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(UINT)calidata.xfinalf,(UINT)calidata.xtempf,
				(UINT)calidata.yfinalf,(UINT)calidata.ytempf,
				(UINT)calidata.zfinalf,(UINT)calidata.ztempf
				);
#endif






	if(calidata.xfinalf==0){
		Write_Input(calidata.xaddoff, 0x3f);//cali mis under off=0x3f
		Write_Input(0x10, 0); //tilt clear
		calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(calidata.xaddmis, (*(calidata.MisDataSpaceConvert))(255)); // x mis to max
	}
	if(calidata.yfinalf==0){
		Write_Input(calidata.yaddoff, 0x3f);//cali mis under off=0x3f
		Write_Input(0x11, 0); //tilt clear
		//calidata.MisDataSpaceConvert = forword_MisDataSpaceConvert;
		calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(calidata.yaddmis, (*(calidata.MisDataSpaceConvert))(255)); // y mis to max
	}
	if(calidata.zfinalf==0){
		Write_Input(calidata.zaddoff, 0x3f);//cali mis under off=0x3f
		Write_Input(0x12, 0); //tilt clear
		calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(calidata.zaddmis, (*(calidata.MisDataSpaceConvert))(255)); // z mis to max
	}

	Read_Output_3axis(acc_buf);
	calidata.xpoutput=calidata.xfoutput=(signed int)((signed char)acc_buf[1])-x;
	calidata.ypoutput=calidata.yfoutput=(signed int)((signed char)acc_buf[3])-y;
	calidata.zpoutput=calidata.zfoutput=(signed int)((signed char)acc_buf[5])-z;
	if((calidata.xpoutput<-25)||(calidata.ypoutput<-25)||(calidata.zpoutput<-25)){
		printk("step2:=file=%s,line=%d  calibration failed\n",__FILE__,__LINE__);
		printk("calidata.xpoutput = 0x%4x  calidata.ypoutput = 0x%4x  calidata.zpoutput = 0x%4x\n",calidata.xpoutput,calidata.ypoutput,calidata.zpoutput);
		return 0;
	}


	if(calidata.xfinalf==0){
		calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(calidata.xaddmis, (*(calidata.MisDataSpaceConvert))(0));
	}
	if(calidata.yfinalf==0){
		//calidata.MisDataSpaceConvert = forword_MisDataSpaceConvert;
		calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(calidata.yaddmis, (*(calidata.MisDataSpaceConvert))(0));
	}
	if(calidata.zfinalf==0){
		calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(calidata.zaddmis, (*(calidata.MisDataSpaceConvert))(0));
	}
	Read_Output_3axis(acc_buf);
	calidata.xnoutput=calidata.xfoutput=(signed int)((signed char)acc_buf[1])-x;
	calidata.ynoutput=calidata.yfoutput=(signed int)((signed char)acc_buf[3])-y;
	calidata.znoutput=calidata.zfoutput=(signed int)((signed char)acc_buf[5])-z;
	if((calidata.xnoutput>25)||(calidata.ynoutput>25)||(calidata.znoutput>25)){
		printk("step2:=file=%s,line=%d  calibration failed\n",__FILE__,__LINE__);
		printk("calidata.xnoutput = 0x%4x  calidata.ynoutput = 0x%4x  calidata.znoutput = 0x%4x\n",calidata.xnoutput,calidata.ynoutput,calidata.znoutput);
		return 0;
	}


	if(abs(calidata.xpoutput)<=abs(calidata.xnoutput)){
		calidata.xfoutput=calidata.xpoutput;
		calidata.xmmis=255;
	}
	else{
		calidata.xfoutput=calidata.xnoutput;
		calidata.xmmis=0;
	}
	if(abs(calidata.ypoutput)<=abs(calidata.ynoutput)){
		calidata.yfoutput=calidata.ypoutput;
		calidata.ymmis=255;
	}
	else{
		calidata.yfoutput=calidata.ynoutput;
		calidata.ymmis=0;
	}
	if(abs(calidata.zpoutput)<=abs(calidata.znoutput)){
		calidata.zfoutput=calidata.zpoutput;
		calidata.zmmis=255;
	}
	else{
		calidata.zfoutput=calidata.znoutput;
		calidata.zmmis=0;
	}

	if(calidata.xfinalf==0){
		calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(calidata.xaddmis, (*(calidata.MisDataSpaceConvert))(calidata.xmmis));
	}
	if(calidata.yfinalf==0){
		//calidata.MisDataSpaceConvert = forword_MisDataSpaceConvert;
		calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(calidata.yaddmis, (*(calidata.MisDataSpaceConvert))(calidata.ymmis));
	}
	if(calidata.zfinalf==0){
		calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
		Write_Input(calidata.zaddmis, (*(calidata.MisDataSpaceConvert))(calidata.zmmis));
	}
	check_output_set_finalflag(&calidata,2);
	//if the smaller output <25,the step3 mis is finished
	if(abs(calidata.xfoutput)<25) calidata.xtempf=1;
	if(abs(calidata.yfoutput)<25) calidata.ytempf=1;
	if(abs(calidata.zfoutput)<25) calidata.ztempf=1;

	calidata.xpmis=calidata.ypmis=calidata.zpmis=255;
	calidata.xnmis=calidata.ynmis=calidata.znmis=0;
	check_finalflag_set_tempflag(&calidata);
#ifdef PRINT
	printk("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(unsigned int)calidata.xfinalf,(unsigned int)calidata.xtempf,
				(unsigned int)calidata.yfinalf,(unsigned int)calidata.ytempf,
				(unsigned int)calidata.zfinalf,(unsigned int)calidata.ztempf
				);
#endif
	cyclecount=0;
	while(1){
		if(++cyclecount>20)break;//if some errs happened,the cyclecount exceeded

		if((calidata.xtempf)&&(calidata.ytempf)&&(calidata.ztempf))break;
		updata_midmis_address(&calidata);
		Read_Output_3axis(acc_buf);
		calidata.xfoutput=(signed int)((signed char)acc_buf[1])-x;
		calidata.yfoutput=(signed int)((signed char)acc_buf[3])-y;
		calidata.zfoutput=(signed int)((signed char)acc_buf[5])-z;
#ifdef PRINT
		printk("xp%4d=%4d,xm%4d=%4d,xn%4d=%4d,      yp%4d=%4d,ym%4d=%4d,yn%4d=%4d,      zp%4d=%4d,zm%4d=%4d,zn%4d=%4d\n\r",
				calidata.xpoutput,(unsigned int)calidata.xpmis,
				calidata.xfoutput,(unsigned int)calidata.xmmis,
				calidata.xnoutput,(unsigned int)calidata.xnmis,
				calidata.ypoutput,(unsigned int)calidata.ypmis,
				calidata.yfoutput,(unsigned int)calidata.ymmis,
				calidata.ynoutput,(unsigned int)calidata.ynmis,
				calidata.zpoutput,(unsigned int)calidata.zpmis,
				calidata.zfoutput,(unsigned int)calidata.zmmis,
				calidata.znoutput,(unsigned int)calidata.znmis
				);
#endif
		updata_mmis_pnfoutput_set_tempflag(&calidata,acc_buf,x,y,z);
		check_output_set_finalflag(&calidata,2);
		if(check_flag_is_return(&calidata))return 1;
	}
#ifdef PRINT
	printk("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(unsigned int)calidata.xfinalf,(unsigned int)calidata.xtempf,
				(unsigned int)calidata.yfinalf,(unsigned int)calidata.ytempf,
				(unsigned int)calidata.zfinalf,(unsigned int)calidata.ztempf
				);
#endif

	calidata.xtempf=calidata.ytempf=calidata.ztempf=1;
	if((calidata.xmmis>0)&&(calidata.xmmis<255))calidata.xtempf=0;
	if((calidata.ymmis>0)&&(calidata.ymmis<255))calidata.ytempf=0;
	if((calidata.zmmis>0)&&(calidata.zmmis<255))calidata.ztempf=0;
	calidata.xpmis=calidata.xnmis=calidata.xmmis;
	calidata.ypmis=calidata.ynmis=calidata.ymmis;
	calidata.zpmis=calidata.znmis=calidata.zmmis;
	for(count = 0; count < 3; count++)
	{
		if(calidata.xtempf==0){
			calidata.xpmis = calidata.xmmis + count - 1;
			if((calidata.xpmis>calidata.xmmis)&&(calidata.xpmis==128))calidata.xpmis = calidata.xmmis + count-1 + 1;
			if((calidata.xpmis<calidata.xmmis)&&(calidata.xpmis==127))calidata.xpmis = calidata.xmmis + count-1 - 1;
			calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
			Write_Input(calidata.xaddmis, (*(calidata.MisDataSpaceConvert))(calidata.xpmis));
		}
		if(calidata.ytempf==0){
			calidata.ypmis = calidata.ymmis + count - 1;
			if((calidata.ypmis>calidata.ymmis)&&(calidata.ypmis==128))calidata.ypmis = calidata.ymmis + count-1 + 1;
			if((calidata.ypmis<calidata.ymmis)&&(calidata.ypmis==127))calidata.ypmis = calidata.ymmis + count-1 - 1;
			//calidata.MisDataSpaceConvert = forword_MisDataSpaceConvert;
			calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
			Write_Input(calidata.yaddmis, (*(calidata.MisDataSpaceConvert))(calidata.ypmis));
		}
		if(calidata.ztempf==0){
			calidata.zpmis = calidata.zmmis + count - 1;
			if((calidata.zpmis>calidata.zmmis)&&(calidata.zpmis==128))calidata.zpmis = calidata.zmmis + count-1 + 1;
			if((calidata.zpmis<calidata.zmmis)&&(calidata.zpmis==127))calidata.zpmis = calidata.zmmis + count-1 - 1;
			calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
			Write_Input(calidata.zaddmis, (*(calidata.MisDataSpaceConvert))(calidata.zpmis));
		}
		Read_Output_3axis(acc_buf);
		if(abs((signed int)((signed char)acc_buf[1])-x)<abs(calidata.xfoutput)){
			calidata.xnmis=calidata.xpmis;
			calidata.xfoutput= (signed int)((signed char)acc_buf[1])-x;
		}
		if(abs((signed int)((signed char)acc_buf[3])-y)<abs(calidata.yfoutput)){
			calidata.ynmis=calidata.ypmis;
			calidata.yfoutput= (signed int)((signed char)acc_buf[3])-y;
		}
		if(abs((signed int)((signed char)acc_buf[5])-z)<abs(calidata.zfoutput)){
			calidata.znmis=calidata.zpmis;
			calidata.zfoutput= (signed int)((signed char)acc_buf[5])-z;
		}
		if(calidata.xtempf==0){
			calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
			Write_Input(calidata.xaddmis, (*(calidata.MisDataSpaceConvert))(calidata.xnmis));
		}
		if(calidata.ytempf==0){
			//calidata.MisDataSpaceConvert = forword_MisDataSpaceConvert;
			calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
			Write_Input(calidata.yaddmis, (*(calidata.MisDataSpaceConvert))(calidata.ynmis));
		}
		if(calidata.ztempf==0){
			calidata.MisDataSpaceConvert = reverse_MisDataSpaceConvert;
			Write_Input(calidata.zaddmis, (*(calidata.MisDataSpaceConvert))(calidata.znmis));
		}
#ifdef PRINT
		printk("L%4d:xf=%4d,xmis=%4d,yf=%4d,ymis=%4d,zf=%4d,zmis=%4d\n\r",__LINE__,
					(signed int)((signed char)acc_buf[1])-x,(unsigned int)calidata.xpmis,
					(signed int)((signed char)acc_buf[3])-y,(unsigned int)calidata.ypmis,
					(signed int)((signed char)acc_buf[5])-z,(unsigned int)calidata.zpmis
					);
#endif




	}

	calidata.xpoff=calidata.ypoff=calidata.zpoff=0x7f;
	calidata.xnoff=calidata.ynoff=calidata.znoff=0;
	calidata.xtempf=calidata.ytempf=calidata.ztempf=0;
#ifdef PRINT
	printk("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(unsigned int)calidata.xfinalf,(unsigned int)calidata.xtempf,
				(unsigned int)calidata.yfinalf,(unsigned int)calidata.ytempf,
				(unsigned int)calidata.zfinalf,(unsigned int)calidata.ztempf
				);
#endif
	check_finalflag_set_tempflag(&calidata);
#ifdef PRINT
	printk("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(unsigned int)calidata.xfinalf,(unsigned int)calidata.xtempf,
				(unsigned int)calidata.yfinalf,(unsigned int)calidata.ytempf,
				(unsigned int)calidata.zfinalf,(unsigned int)calidata.ztempf
				);
#endif

	if(calidata.xtempf==0){
		calidata.OffDataSpaceConvert = reverse_OffDataSpaceConvert;
		Write_Input(calidata.xaddoff, (*(calidata.OffDataSpaceConvert))(calidata.xpoff)); // x off to max
	}
	if(calidata.ytempf==0){
		//calidata.OffDataSpaceConvert = forword_OffDataSpaceConvert;
		calidata.OffDataSpaceConvert = reverse_OffDataSpaceConvert;
		Write_Input(calidata.yaddoff, (*(calidata.OffDataSpaceConvert))(calidata.xpoff)); // y off to max
	}
	if(calidata.ztempf==0){
		calidata.OffDataSpaceConvert = forword_OffDataSpaceConvert;
		Write_Input(calidata.zaddoff, (*(calidata.OffDataSpaceConvert))(calidata.xpoff)); // z off to max
	}
	Read_Output_3axis(acc_buf);
	calidata.xpoutput=calidata.xfoutput=(signed int)((signed char)acc_buf[1])-x;
	calidata.ypoutput=calidata.yfoutput=(signed int)((signed char)acc_buf[3])-y;
	calidata.zpoutput=calidata.zfoutput=(signed int)((signed char)acc_buf[5])-z;
#ifdef PRINT
	printk("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(unsigned int)calidata.xfinalf,(unsigned int)calidata.xtempf,
				(unsigned int)calidata.yfinalf,(unsigned int)calidata.ytempf,
				(unsigned int)calidata.zfinalf,(unsigned int)calidata.ztempf
				);
#endif
	check_output_set_finalflag(&calidata,2);
#ifdef PRINT
	printk("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(unsigned int)calidata.xfinalf,(unsigned int)calidata.xtempf,
				(unsigned int)calidata.yfinalf,(unsigned int)calidata.ytempf,
				(unsigned int)calidata.zfinalf,(unsigned int)calidata.ztempf
				);
#endif

	if(calidata.xtempf==0){
		calidata.OffDataSpaceConvert = reverse_OffDataSpaceConvert;
		Write_Input(calidata.xaddoff, (*(calidata.OffDataSpaceConvert))(calidata.xnoff)); // x off to min
	}
	if(calidata.ytempf==0){
		//calidata.OffDataSpaceConvert = forword_OffDataSpaceConvert;
		calidata.OffDataSpaceConvert = reverse_OffDataSpaceConvert;
		Write_Input(calidata.yaddoff, (*(calidata.OffDataSpaceConvert))(calidata.ynoff)); // y off to min
	}
	if(calidata.ztempf==0){
		calidata.OffDataSpaceConvert = forword_OffDataSpaceConvert;
		Write_Input(calidata.zaddoff, (*(calidata.OffDataSpaceConvert))(calidata.znoff)); // z off to min
	}
	Read_Output_3axis(acc_buf);
	calidata.xnoutput=calidata.xfoutput=(signed int)((signed char)acc_buf[1])-x;
	calidata.ynoutput=calidata.yfoutput=(signed int)((signed char)acc_buf[3])-y;
	calidata.znoutput=calidata.zfoutput=(signed int)((signed char)acc_buf[5])-z;
#ifdef PRINT
	printk("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(unsigned int)calidata.xfinalf,(unsigned int)calidata.xtempf,
				(unsigned int)calidata.yfinalf,(unsigned int)calidata.ytempf,
				(unsigned int)calidata.zfinalf,(unsigned int)calidata.ztempf
				);
#endif
	check_output_set_finalflag(&calidata,2);
#ifdef PRINT
	printk("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(unsigned int)calidata.xfinalf,(unsigned int)calidata.xtempf,
				(unsigned int)calidata.yfinalf,(unsigned int)calidata.ytempf,
				(unsigned int)calidata.zfinalf,(unsigned int)calidata.ztempf
				);
#endif
	if(abs(calidata.xpoutput)<=abs(calidata.xnoutput)){
		calidata.xfoutput=calidata.xpoutput;
		calidata.xmoff=calidata.xpoff;
	}
	else{
		calidata.xfoutput=calidata.xnoutput;
		calidata.xmoff=calidata.xnoff;
	}
	if(abs(calidata.ypoutput)<=abs(calidata.ynoutput)){
		calidata.yfoutput=calidata.ypoutput;
		calidata.ymoff=calidata.ypoff;
	}
	else{
		calidata.yfoutput=calidata.ynoutput;
		calidata.ymoff=calidata.ynoff;
	}
	if(abs(calidata.zpoutput)<=abs(calidata.znoutput)){
		calidata.zfoutput=calidata.zpoutput;
		calidata.zmoff=calidata.zpoff;
	}
	else{
		calidata.zfoutput=calidata.znoutput;
		calidata.zmoff=calidata.znoff;
	}
	if(calidata.xtempf==0){
		calidata.OffDataSpaceConvert = reverse_OffDataSpaceConvert;
		Write_Input(calidata.xaddoff, (*(calidata.OffDataSpaceConvert))(calidata.xmoff));
	}
	if(calidata.ytempf==0){
		//calidata.OffDataSpaceConvert = forword_OffDataSpaceConvert;
		calidata.OffDataSpaceConvert = reverse_OffDataSpaceConvert;
		Write_Input(calidata.yaddoff, (*(calidata.OffDataSpaceConvert))(calidata.ymoff));
	}
	if(calidata.ztempf==0){
		calidata.OffDataSpaceConvert = forword_OffDataSpaceConvert;
		Write_Input(calidata.zaddoff, (*(calidata.OffDataSpaceConvert))(calidata.zmoff));
	}
	if((calidata.xpoutput>0 && calidata.xnoutput>0)||(calidata.xpoutput<0 && calidata.xnoutput<0)){
		calidata.xfinalf=1;
	}

	if((calidata.ypoutput>0 && calidata.ynoutput>0)||(calidata.ypoutput<0 && calidata.ynoutput<0)){
		calidata.yfinalf=1;
	}

	if((calidata.zpoutput>0 && calidata.znoutput>0)||(calidata.zpoutput<0 && calidata.znoutput<0)){
		calidata.zfinalf=1;
	}


	check_finalflag_set_tempflag(&calidata);
#ifdef PRINT
	printk("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(unsigned int)calidata.xfinalf,(unsigned int)calidata.xtempf,
				(unsigned int)calidata.yfinalf,(unsigned int)calidata.ytempf,
				(unsigned int)calidata.zfinalf,(unsigned int)calidata.ztempf
				);
#endif
	cyclecount=0;
	while(1){
		if(++cyclecount>20)break;

		if((calidata.xtempf)&&(calidata.ytempf)&&(calidata.ztempf))break;
		updata_midoff_address(&calidata);
		Read_Output_3axis(acc_buf);
		calidata.xfoutput=(signed int)((signed char)acc_buf[1])-x;
		calidata.yfoutput=(signed int)((signed char)acc_buf[3])-y;
		calidata.zfoutput=(signed int)((signed char)acc_buf[5])-z;
#ifdef PRINT
		printk("xp%4d=%4d,xm%4d=%4d,xn%4d=%4d,      yp%4d=%4d,ym%4d=%4d,yn%4d=%4d,      zp%4d=%4d,zm%4d=%4d,zn%4d=%4d\n\r",
				calidata.xpoutput,(unsigned int)calidata.xpoff,
				calidata.xfoutput,(unsigned int)calidata.xmoff,
				calidata.xnoutput,(unsigned int)calidata.xnoff,
				calidata.ypoutput,(unsigned int)calidata.ypoff,
				calidata.yfoutput,(unsigned int)calidata.ymoff,
				calidata.ynoutput,(unsigned int)calidata.ynoff,
				calidata.zpoutput,(unsigned int)calidata.zpoff,
				calidata.zfoutput,(unsigned int)calidata.zmoff,
				calidata.znoutput,(unsigned int)calidata.znoff
				);
#endif
		updata_moff_pnfoutput_set_tempflag(&calidata,acc_buf,x,y,z);
		check_output_set_finalflag(&calidata,2);
		if(check_flag_is_return(&calidata))return 1;
	}
#ifdef PRINT
	printk("L%4d:xff=%4d,xtf=%4d,yff=%4d,ytf=%4d,zff=%4d,ztf=%4d\n\r",__LINE__,
				(unsigned int)calidata.xfinalf,(unsigned int)calidata.xtempf,
				(unsigned int)calidata.yfinalf,(unsigned int)calidata.ytempf,
				(unsigned int)calidata.zfinalf,(unsigned int)calidata.ztempf
				);
#endif



	return 1;
}

int auto_calibration_instant_mtp(signed int x, signed int y, signed int z){
	unsigned char readbuf[3]={0};
	unsigned char buffer[6] = {0};
	char reg_13;

	signed int xoutp,youtp,zoutp;
	unsigned char xfinalf,yfinalf,zfinalf;
	xoutp=youtp=zoutp=0;
	xfinalf=yfinalf=zfinalf=0;
    reg_13= 0;
	Write_Input(0x1e, 0x05);
	if(auto_calibration_instant(x,y,z)==0)return 0;

	//msleep(20);
	tilt_3axis_mtp(x,y,z);
	Read_Output_3axis(buffer);
	xoutp=(signed int)((signed char)buffer[1])-x;
	youtp=(signed int)((signed char)buffer[3])-y;
	zoutp=(signed int)((signed char)buffer[5])-z;

	if(abs(xoutp) < 2){xfinalf=1;}
	if(abs(youtp) < 2){yfinalf=1;}
	if(abs(zoutp) < 2){zfinalf=1;}


	//*tbuffer = 0x10;
	//sensor_rx_data(sc7660_client, tbuffer,1);
	readbuf[0]= Read_Reg(0x10);
	//*tbuffer = 0x40;
	//sensor_rx_data(sc7660_client, tbuffer,1);
	readbuf[1]= Read_Reg(0x40);
	//*tbuffer = 0x47;
	//sensor_rx_data(sc7660_client, tbuffer,1);
	readbuf[2]= Read_Reg(0x47);
	printk("L%4d:xtilt=%4d,xmis=%4d,xoff=%4d\n\r",__LINE__,
			(unsigned int)readbuf[0],
			(unsigned int)readbuf[1],
			(unsigned int)readbuf[2]
			);



	//*tbuffer = 0x11;
	//sensor_rx_data(sc7660_client, tbuffer,1);
	readbuf[0]= Read_Reg(0x11);
	//*tbuffer = 0x41;
	//sensor_rx_data(sc7660_client, tbuffer,1);
	readbuf[1]= Read_Reg(0x41);
	//*tbuffer = 0x48;
	//sensor_rx_data(sc7660_client, tbuffer,1);
	readbuf[2]= Read_Reg(0x48);
	printk("L%4d:ytilt=%4d,ymis=%4d,yoff=%4d\n\r",__LINE__,
			(unsigned int)readbuf[0],
			(unsigned int)readbuf[1],
			(unsigned int)readbuf[2]
			);


	readbuf[0]= Read_Reg(0x12);

	readbuf[1]= Read_Reg(0x42);

	readbuf[2]= Read_Reg(0x49);
	printk("L%4d:ztilt=%4d,zmis=%4d,zoff=%4d\n\r",__LINE__,
			(unsigned int)readbuf[0],
			(unsigned int)readbuf[1],
			(unsigned int)readbuf[2]
			);

	if(xfinalf && yfinalf && zfinalf)
	{
		Write_Input(0x13,0x01);//allen MTP
		reg_13 = Read_Reg(0x13);
		printk("line %d  reg_13 = %x\n",__LINE__,reg_13);
		Write_Input(0x1e, 0x15);
		mdelay(300);
		reg_13 = Read_Reg(0x13);
		printk("line %d  reg_13 = %x\n",__LINE__,reg_13);
		Write_Input(0x1e, 05);

		printk(KERN_INFO "run calibration finished\n");

		return 1;
  	}
	else
		return 0;
}




static ssize_t SC7660_3_axis_Calibration(struct device *dev, struct device_attribute *attr ,const char *buf, size_t count)
{
	int adj_ok;
		Write_Input(0x22, 0x77);
	Write_Input(0x1e, 0x05);   //supply the rivilege for MTP
	mdelay(100);
	adj_ok = auto_calibration_instant_mtp(0,0,-64);
    if(adj_ok )
   	{
		Write_Input(0x1e, 0x15);  //save the modifyed register value
		mdelay(200);
        Write_Input(0x1e, 0);
		printk(KERN_INFO "run calibration success\n");
	}
	else
	{
		printk(KERN_INFO "run calibration failed\n");
	}
     Write_Input(0x22, 0x47);
    	mdelay(5);
	return 1;
}


/**
 * gsensor_detect - Device detection callback for automatic device creation
 * return value:
 *                    = 0; success;
 *                    < 0; err
 */
static int sc7660_gsensor_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;
	//__s32 ret = 0;

        if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
                return -ENODEV;

	    if(twi_id == adapter->nr){
                printk("%s: addr= %x\n",__func__,client->addr);
/*
            ret = gsensor_i2c_test(client);
        	if(!ret){
        		pr_info("%s:I2C connection might be something wrong or maybe the other gsensor equipment! \n",__func__);
        		return -ENODEV;
        	}else
*/
			{
            	    dprintk(DEBUG_INIT, "%s: I2C connection sucess!\n", __func__);
            	    strlcpy(info->type, SENSOR_NAME, I2C_NAME_SIZE);
    		        return 0;
	        }

	}else{
        dprintk(DEBUG_INIT, "%s: I2C connection error!\n", __func__);
		return -ENODEV;
	}
}
static ssize_t sc7660_delay_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = sc7660_i2c_client;
	struct sc7660_data_s *sc7660 = NULL;

	sc7660 = i2c_get_clientdata(client);
	return sprintf(buf, "%d\n", sc7660->pollDev->poll_interval);
}

static ssize_t sc7660_delay_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long data;
	//int error;
	struct i2c_client *client       = sc7660_i2c_client;
	struct sc7660_data_s *sc7660  = NULL;

        sc7660    = i2c_get_clientdata(client);
        dprintk(DEBUG_INIT, "delay store %d\n", __LINE__);

	data = simple_strtoul(buf, NULL, 10);
	//error = strict_strtoul(buf, 10, &data);
	//if (error)
	//	return error;

	if (data > POLL_INTERVAL_MAX)
		data = POLL_INTERVAL_MAX;

        mutex_lock(&sc7660->interval_mutex);
		// if the sensor sample rate is too fast, the real report rate
		// maybe lower than sample rate when system is busy. reduce the sample
		// time to raise report rate.
        sc7660->pollDev->poll_interval = data;
        mutex_unlock(&sc7660->interval_mutex);

		if (atomic_read(&sc7660_data.enable)) {
			if (sc7660->pollDev->poll_interval <= 10) {
				sc7660_data.ktime = ktime_set(0, sc7660->pollDev->poll_interval * NSEC_PER_MSEC - NSEC_PER_MSEC / 2);
			} else {
				sc7660_data.ktime = ktime_set(0, sc7660->pollDev->poll_interval * NSEC_PER_MSEC);
			}
			hrtimer_start(&sc7660_data.hr_timer, sc7660_data.ktime, HRTIMER_MODE_REL);
		}

	return count;
}

static ssize_t sc7660_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct i2c_client *client       = sc7660_i2c_client;
	struct sc7660_data_s *sc7660  = NULL;

	sc7660    = i2c_get_clientdata(client);

	return sprintf(buf, "%d\n",atomic_read(&sc7660_data.enable));

}

static ssize_t sc7660_enable_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long data;
	s32 val = 0;
	int error;

	struct i2c_client *client       = sc7660_i2c_client;
	struct sc7660_data_s *sc7660  = NULL;

	sc7660    = i2c_get_clientdata(client);

    printk(KERN_INFO "%s: buf=%s\n", __func__, buf);
	data = simple_strtoul(buf, NULL, 10);
	//error = strict_strtoul(buf, 10, &data);

	//if(error) {
	//	pr_err("%s strict_strtoul error\n", __FUNCTION__);
	//	goto exit;
	//}

	if(data) {
    	printk(KERN_INFO "%s: sc7660_CTRL_PWRON\n", __func__);
		error = i2c_smbus_write_byte_data(sc7660_i2c_client, sc7660_REG_CTRL,sc7660_CTRL_PWRON);
		atomic_set(&sc7660_data.enable, 1);
		assert(error==0);
		// set sensor max range is 4G,because cts 10_r1 request
		val = i2c_smbus_read_byte_data(sc7660_i2c_client, sc7660_REG_CTRL + 3);
		val &= ~(sc7660_MEASURING_RANGE);
		error = i2c_smbus_write_byte_data(sc7660_i2c_client, sc7660_REG_CTRL_RANGE, val | sc7660_MEASURING_RANGE_4G);
		assert(error == 0);
		//
		printk(KERN_INFO "%s: sc7660 delay %d\n", __func__, sc7660->pollDev->poll_interval);
		if (sc7660->pollDev->poll_interval <= 10) {
			sc7660_data.ktime = ktime_set(0, sc7660->pollDev->poll_interval * NSEC_PER_MSEC - NSEC_PER_MSEC / 2);
		} else {
			sc7660_data.ktime = ktime_set(0, sc7660->pollDev->poll_interval * NSEC_PER_MSEC);
		}
		sc7660_data.ktime = ktime_set(0, sc7660->pollDev->poll_interval * NSEC_PER_MSEC);
		hrtimer_start(&sc7660_data.hr_timer, ktime_set(0, 0), HRTIMER_MODE_REL);
		//sc7660_idev->input->open(sc7660_idev->input);
	} else {
    	printk(KERN_INFO "%s: sc7660_CTRL_PWRDN\n", __func__);
			//sc7660_idev->input->close(sc7660_idev->input);
			hrtimer_cancel(&sc7660_data.hr_timer);
			atomic_set(&sc7660_data.enable, 0);
		error = i2c_smbus_write_byte_data(sc7660_i2c_client, sc7660_REG_CTRL,sc7660_CTRL_PWRDN);
		assert(error==0);
	}

	return count;

//exit:
//	return error;
}

#if 0
static ssize_t SC7660_reg13_reset(struct device *dev,
        struct device_attribute *attr,
        const char *buf, size_t count)
{

    int result = 0;
        result = i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1e,0x05);
		mdelay(100);
		result = i2c_smbus_write_byte_data(sc7660_i2c_client, 0x13, 0x00);

		 result = i2c_smbus_write_byte_data(sc7660_i2c_client,0x1e, 0x15);
		mdelay(300);

    if(result)
		printk("%s:fail to write sensor_register\n",__func__);

    return count;
}
#endif

static ssize_t SC7660_register_store(struct device *dev,
        struct device_attribute *attr,
        const char *buf, size_t count)
{
    unsigned int address, value;
    int result = 0;
    sscanf(buf, "0x%x=0x%x", &address, &value);

    result = i2c_smbus_write_byte_data(sc7660_i2c_client, address,value);

    if(result)
		printk("%s:fail to write sensor_register\n",__func__);

    return count;
}

static ssize_t SC7660_register_show(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    size_t count = 0;
    //u8 reg[0x5b];
    char i;
	int buffer[3] = {0};
	i = 0x0f;
    *buffer = i;
    buffer[0] = i2c_smbus_read_byte_data(sc7660_i2c_client,i );
	count += sprintf(buf, "0x%x: 0x%x\n", i, buffer[0]);
	for(i=0x10;i<0x5a;i++)
	{
		*buffer = i;
		buffer[0]=i2c_smbus_read_byte_data(sc7660_i2c_client, i);
		count += sprintf(&buf[count],"0x%x: 0x%x\n", i, buffer[0]);
	}
    return count;
}
#if 0
static DEVICE_ATTR(enable, S_IRUGO,
		sc7660_enable_show, sc7660_enable_store);

static DEVICE_ATTR(delay, S_IRUGO,
		sc7660_delay_show,  sc7660_delay_store);

static DEVICE_ATTR(reg, S_IRUGO,
		SC7660_register_show,  SC7660_register_store);

static DEVICE_ATTR(reg13_reset, S_IRUGO,
		SC7660_reg13_reset,  NULL);

static DEVICE_ATTR(calibration_run, S_IRUGO,//S_IWUSR|S_IWGRP, //calibration_run
        NULL, SC7660_3_axis_Calibration);
#endif
static DEVICE_ATTR(enable, 0644,
		sc7660_enable_show, sc7660_enable_store);

static DEVICE_ATTR(delay, 0644,
		sc7660_delay_show,  sc7660_delay_store);

static DEVICE_ATTR(reg, 0644,
		SC7660_register_show,  SC7660_register_store);

//static DEVICE_ATTR(reg13_reset, 0644,
//		SC7660_reg13_reset,  NULL);

static DEVICE_ATTR(calibration_run, 0644,//S_IWUSR|S_IWGRP, //calibration_run
        NULL, SC7660_3_axis_Calibration);

static struct attribute *sc7660_attributes[] = {
//	&dev_attr_reg13_reset.attr,
	&dev_attr_reg.attr,
	&dev_attr_enable.attr,
	&dev_attr_delay.attr,
	&dev_attr_calibration_run.attr,
	NULL
};

static struct attribute_group sc7660_attribute_group = {
	.attrs = sc7660_attributes
};

#if 0
static int report_abs(void)
{
	s8 xyz[6] = {0, 0, 0};
	s16   x   = 0, y = 0, z = 0;
	s8   ret  = 0;
    s16 conut = 0;
	while(1)
	{

		ret = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_STATUS);
		if ((ret& 0x08) != 0  ){
			break;
		}
		msleep(1);
		conut++;
		if(conut > 40)
			break;

	}

	xyz[0] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_XOUT_L);
	xyz[1] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_XOUT_H);

	xyz[2] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_YOUT_L);
	xyz[3] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_YOUT_H);

	xyz[4] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_ZOUT_L);
	xyz[5] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_ZOUT_H);

	//x = ((xyz[1]) << 8) | xyz[0];
	//y = ((xyz[3]) << 8) | xyz[2];
	//z = ((xyz[5]) << 8) | xyz[4];

	x = xyz[1];
	y = xyz[3];
	z = xyz[5];


    input_report_abs(sc7660_idev->input, ABS_X, x);
    input_report_abs(sc7660_idev->input, ABS_Y, y);
    input_report_abs(sc7660_idev->input, ABS_Z, z);

	input_sync(sc7660_idev->input);
	return 1;
}

#endif

static void sc7660_dev_poll(struct input_polled_dev *dev)
{
#ifdef CONFIG_HAS_EARLYSUSPEND
	if(0 == sc7660_data.suspend_indator){
		//report_abs();
	}
#else
	{
		//report_abs();
	}
#endif
}

/*
 * I2C init/probing/exit functions
 */

////////////////////////////////djq////////////////////////////////////
static int sc7660_acc_get_data( int *xyz)
{
	u8 acc_data[6];
	/* x,y,z hardware data */
	int hw_d[3] = { 0 };
	signed short temp_data[3];
	int count = 0;
	u8 buf[3];
	buf[0] = SC7660_STATUS;
	while(1)
	{

		buf[0]=i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_STATUS);
		if ((buf[0] & 0x08) != 0  ){
			break;
		}
		msleep(1);
		count++;
		if(count > 40)
			break;
	}


	buf[0]=i2c_smbus_read_byte_data(sc7660_i2c_client,SC7660_XOUT_L);
	acc_data[0] = buf[0];
	buf[0]=i2c_smbus_read_byte_data(sc7660_i2c_client,SC7660_XOUT_H);

	acc_data[1] = buf[0];


    buf[0]=i2c_smbus_read_byte_data(sc7660_i2c_client,SC7660_YOUT_L);
	acc_data[2] = buf[0];
	buf[0]=i2c_smbus_read_byte_data(sc7660_i2c_client,SC7660_YOUT_H);
	acc_data[3] = buf[0];


    buf[0]=i2c_smbus_read_byte_data(sc7660_i2c_client,SC7660_ZOUT_L);
	acc_data[4] = buf[0];
	buf[0]=i2c_smbus_read_byte_data(sc7660_i2c_client,SC7660_ZOUT_H);
	acc_data[5] = buf[0];

	/* 12-bit */
	temp_data[0] = (signed short)( (acc_data[1]   << 8 | acc_data[0]) >> 4 );
	temp_data[1] = (signed short)( (acc_data[3]   << 8 | acc_data[2]) >> 4 );
	temp_data[2] = (signed short)( (acc_data[5]   << 8 | acc_data[4]) >> 4 );

	hw_d[0] = (temp_data[0] & 0x0800) ? (temp_data[0] | 0xFFFFF800) : (temp_data[0]);
	hw_d[1] = (temp_data[1] & 0x0800) ? (temp_data[1] | 0xFFFFF800) : (temp_data[1]);
	hw_d[2] = (temp_data[2] & 0x0800) ? (temp_data[2] | 0xFFFFF800) : (temp_data[2]);



	xyz[0] = (hw_d[0] )  * 1000;
	xyz[1] = (hw_d[1] )  * 1000;
	xyz[2] = (hw_d[2] )  * 1000;

	return 0;
}


static int sc7660_acc_hw_init(void)
{
	char ret;
	ret = i2c_smbus_write_byte_data(sc7660_i2c_client, SC7660_MODE, 0x77);
	if(ret < 0)
    {
		return -1;
    }

	return 0;
}

static int sc7660_acc_cali(struct i2c_client *client)
{
	struct sc7660_data_s *data = (struct sc7660_data_s *) i2c_get_clientdata(client);

	queue_delayed_work(sc7660_workqueue,
			&data->dwork,
			msecs_to_jiffies(1000));

	printk("cali !!!\n");
	sc7660_acc_hw_init();
	return 0;
}


static void sc7660_work_handler(struct work_struct *work)
{

	int xyz[3] = {0, 0, 0};
	int ret =0;
	int adj_ok;

	int auto_count = 0;
 	int max_x=0, max_y=0, max_z=0, min_x=0, min_y=0, min_z=0;
 	int sum_x = 0, sum_y = 0, sum_z = 0;

	struct sc7660_data_s *data =
		container_of(work, struct sc7660_data_s, dwork.work);

	while (1) {

		ret = sc7660_acc_get_data(xyz);
		if(ret == -1)
		{
			printk("Line %d:error!\n",__LINE__);
			continue;
		}

		xyz[0] /= 1024;
		xyz[1] /= 1024;
		xyz[2] /= 1024;

		if (auto_count == 0) {
			max_x = min_x = xyz[0];
			max_y = min_y = xyz[1];
			max_z = min_z = xyz[2];
		}

		max_x = max_x > xyz[0] ? max_x : xyz[0];
		max_y = max_y > xyz[1] ? max_y : xyz[1];
		max_z = max_z > xyz[2] ? max_z : xyz[2];
		min_x = min_x > xyz[0] ? xyz[0] : min_x;
		min_y = min_y > xyz[1] ? xyz[1] : min_y;
		min_z = min_z > xyz[2] ? xyz[2] : min_z;

		sum_x = sum_x + xyz[0];
		sum_y = sum_y + xyz[1];
		sum_z = sum_z + xyz[2];


		printk("x : %d  y : %d z : %d   sum_z = %d,auto_count = %d\n",xyz[0], xyz[1], xyz[2],sum_z, auto_count);

		if (auto_count > CALIBRATION_NUM-1) {
			printk("x-x : %d\n", max_x - min_x);
			printk("y-y : %d\n", max_y - min_y);
			printk("z-z : %d\n", max_z - min_z);

			if (max_x - min_x < AXIS_X_Y_RANGE_LIMIT
					&& max_y - min_y < AXIS_X_Y_RANGE_LIMIT
					&& max_z - min_z < AXIS_X_Y_RANGE_LIMIT ) {
				printk("sum-x-before : %d\n", sum_x);
				printk("sum-y-before : %d\n", sum_y);
				printk("sum-z-before : %d\n", sum_z);

				//ok
				sum_x /= CALIBRATION_NUM;
				sum_y /= CALIBRATION_NUM;
				sum_z /= CALIBRATION_NUM;
				printk("sum-x : %d\n", sum_x);
				printk("sum-y : %d\n", sum_y);
				printk("sum-z : %d\n", sum_z);

				//if (sum_x < AXIS_X_Y_AVG_LIMIT && sum_y < AXIS_X_Y_AVG_LIMIT) {
					if(1){
					if (sum_z > AXIS_Z_RANGE) {
						//auto_SC7660_3_axis_Calibration( 0, 0, 64);
						adj_ok = auto_calibration_instant_mtp(0,0,64);
						auto_count = GOTO_CALI;
					}
					else if ( sum_z < -AXIS_Z_RANGE) {
						//auto_SC7660_3_axis_Calibration( 0, 0, -64);
						adj_ok = auto_calibration_instant_mtp(0,0,-64);
						auto_count = GOTO_CALI;
					}
					else {
						printk("Line %d:error!\n",__LINE__);
						auto_count = FAILTO_CALI;
					}
				}
				else if( sum_x < AXIS_X_Y_AVG_LIMIT || sum_y < AXIS_X_Y_AVG_LIMIT) {
					if ( sum_z > AXIS_Z_DFT_G - AXIS_Z_RANGE && sum_z < AXIS_Z_DFT_G + AXIS_Z_RANGE) {
						//auto_SC7660_3_axis_Calibration( 0, 0, 64);
						auto_count = GOTO_CALI;
					}
					else if ( sum_z < -(AXIS_Z_DFT_G - AXIS_Z_RANGE) && sum_z > -(AXIS_Z_DFT_G + AXIS_Z_RANGE)) {
						//auto_SC7660_3_axis_Calibration( 0, 0, -64);
						auto_count = GOTO_CALI;
					}
					else {
						auto_count = FAILTO_CALI;
						printk("Line %d:error!\n",__LINE__);
					}
				}
				else {
					auto_count = FAILTO_CALI;
					printk("Line %d:error!\n",__LINE__);
				}
				if(adj_ok==1)
					{
						printk("Calibration succeed\n");
						Write_Input(  0x1e, 0x05);
					}
					else
					{
						printk("Calibration failed\n");
					}
			}
			else {
				auto_count = FAILTO_CALI;
				printk("Line %d:error!\n",__LINE__);
			}
		}

		if(auto_count == GOTO_CALI){
			break;
		}

		if (auto_count == FAILTO_CALI) {
			sum_x = sum_y = sum_z = max_x =  max_y =  max_z =  min_x =  min_y =  min_z = 0;
			auto_count = -1;
			data->time_of_cali++;
			if (data->time_of_cali > 1) {
				printk("calibration failed !!\n");
				break;
			}
		}
		auto_count++;
		msleep(20);

	}
}

static enum hrtimer_restart my_hrtimer_callback(struct hrtimer *timer)
{
	schedule_work(&sc7660_data.wq_hrtimer);
	hrtimer_forward_now(&sc7660_data.hr_timer, sc7660_data.ktime);
	return HRTIMER_RESTART;
}

static void wq_func_hrtimer(struct work_struct *work)
{
	s8 xyz[6] = {0, 0, 0};
	s16   x   = 0, y = 0, z = 0;

	xyz[0] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_XOUT_L);
	xyz[1] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_XOUT_H);
	xyz[2] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_YOUT_L);
	xyz[3] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_YOUT_H);
	xyz[4] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_ZOUT_L);
	xyz[5] = i2c_smbus_read_byte_data(sc7660_i2c_client, SC7660_ZOUT_H);

	x = xyz[1];
	y = xyz[3];
	z = xyz[5];

	if (x == 0 && y == 0 && z == 0) {
		printk("sc7660 gsensor x y z all 0!!!\n");
		sc7660_reset();
		return;
	}
	input_report_abs(sc7660_idev->input, ABS_X, x);
	input_report_abs(sc7660_idev->input, ABS_Y, y);
	input_report_abs(sc7660_idev->input, ABS_Z, z);
	input_sync(sc7660_idev->input);
}

static int sc7660_probe(struct i2c_client *client,
				   const struct i2c_device_id *id)
{
	char     reg_cali;

	int result;
	struct i2c_adapter *adapter;
    struct sc7660_data_s* data = &sc7660_data;

	printk( "sc7660 probe\n");
	sc7660_i2c_client = client;
	adapter = to_i2c_adapter(client->dev.parent);
 	result = i2c_check_functionality(adapter,
 					 I2C_FUNC_SMBUS_BYTE |
 					 I2C_FUNC_SMBUS_BYTE_DATA);
	assert(result);

	hwmon_dev = hwmon_device_register(&client->dev);
	assert(!(IS_ERR(hwmon_dev)));

	/*input poll device register */
	sc7660_idev = input_allocate_polled_device();
	if (!sc7660_idev) {
		dev_err(&client->dev, "alloc poll device failed!\n");
		result = -ENOMEM;
		return result;
	}
	sc7660_idev->poll = sc7660_dev_poll;
	sc7660_idev->poll_interval = POLL_INTERVAL;
	sc7660_idev->poll_interval_max = POLL_INTERVAL_MAX;
	sc7660_idev->input->name = sc7660_DRV_NAME;
	sc7660_idev->input->id.bustype = BUS_I2C;
	sc7660_idev->input->evbit[0] = BIT_MASK(EV_ABS);
    mutex_init(&data->interval_mutex);
	input_set_capability(sc7660_idev->input, EV_ABS, ABS_MISC);
	input_set_abs_params(sc7660_idev->input, ABS_X, -512, 512, INPUT_FUZZ, INPUT_FLAT);
	input_set_abs_params(sc7660_idev->input, ABS_Y, -512, 512, INPUT_FUZZ, INPUT_FLAT);
	input_set_abs_params(sc7660_idev->input, ABS_Z, -512, 512, INPUT_FUZZ, INPUT_FLAT);

	result = input_register_polled_device(sc7660_idev);
	if (result) {
		dev_err(&client->dev, "register poll device failed!\n");
		return result;
	}
	result = sysfs_create_group(&sc7660_idev->input->dev.kobj, &sc7660_attribute_group);

	if(result) {
		dev_err(&client->dev, "create sys failed\n");
	}

    data->client  = client;
    data->pollDev = sc7660_idev;
	i2c_set_clientdata(client, data);

#ifdef CONFIG_HAS_EARLYSUSPEND
	sc7660_data.early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	sc7660_data.early_suspend.suspend = sc7660_early_suspend;
	sc7660_data.early_suspend.resume = sc7660_late_resume;
	register_early_suspend(&sc7660_data.early_suspend);
	sc7660_data.suspend_indator = 0;
#endif

    //result = i2c_smbus_write_byte_data(sc7660_i2c_client, sc7660_REG_CTRL, sc7660_CTRL_PWRON);
	//assert(result==0);

	reg_cali = i2c_smbus_read_byte_data(sc7660_i2c_client, 0x13);
	printk("line %d reg_cali = 0x%x \n",__LINE__,reg_cali);


  if (0) {
	//if ((reg_cali & 0x1) == 0) {

		if (sc7660_workqueue == NULL) {
			sc7660_workqueue = create_workqueue("sc7660_acc");
			if (sc7660_workqueue == NULL) {
		    		printk("line %d workque fail!!!!!!!!!!!!!!\n",__LINE__);
			}
		}


		INIT_DELAYED_WORK(&(data->dwork), sc7660_work_handler);
		sc7660_acc_cali(client);
	}
	else {

	}

	hrtimer_init(&sc7660_data.hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sc7660_data.hr_timer.function = my_hrtimer_callback;
	INIT_WORK(&sc7660_data.wq_hrtimer, wq_func_hrtimer);

	return result;
}

static int sc7660_remove(struct i2c_client *client)
{
	int result;
    result = i2c_smbus_write_byte_data(sc7660_i2c_client, sc7660_REG_CTRL, sc7660_CTRL_PWRDN);
	assert(result==0);
	hwmon_device_unregister(hwmon_dev);
	#ifdef CONFIG_HAS_EARLYSUSPEND
	  unregister_early_suspend(&sc7660_data.early_suspend);
	#endif
	sysfs_remove_group(&sc7660_idev->input->dev.kobj, &sc7660_attribute_group);
	input_unregister_polled_device(sc7660_idev);
	input_free_polled_device(sc7660_idev);
	i2c_set_clientdata(sc7660_i2c_client, NULL);

	return result;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void sc7660_early_suspend(struct early_suspend *h)
{
	int result;
	printk( "sc7660 early suspend\n");
	sc7660_data.suspend_indator = 1;
	result = i2c_smbus_write_byte_data(sc7660_i2c_client, sc7660_REG_CTRL, sc7660_CTRL_PWRDN);
	assert(result==0);
#ifdef GSENSOR_REG_CALI
	{
		int i = 0;
		for(i = 0; i < sizeof(reg)/sizeof(reg[0]); i++) {
			reg_data[i] = i2c_smbus_read_byte_data(sc7660_i2c_client, reg[i]);
		}
	}
#endif
	return;
}

static void sc7660_late_resume(struct early_suspend *h)
{
	int result;
	int val_reg;
	int MTPSETTING,B57H,B1BH,i;
	printk("sc7660 late resume\n");
	sc7660_data.suspend_indator = 0;
    val_reg = i2c_smbus_read_byte_data(sc7660_i2c_client,0x0f);
	if(val_reg != 0x11)
	{
		for(i=0;i<3;i++)
		{
		MTPSETTING = 0x07;
		B57H = 0x00;
		B1BH = 0x08;
		sc7660_i2c_client->addr=0x18;
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x59,MTPSETTING);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1e,0x05);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1b,B1BH);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x57,B57H);
		sc7660_i2c_client->addr=0x19;
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x59,MTPSETTING);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1e,0x05);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1b,B1BH);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x57,B57H);
		sc7660_i2c_client->addr=0x1a;
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x59,MTPSETTING);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1e,0x05);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1b,B1BH);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x57,B57H);
		sc7660_i2c_client->addr=0x1b;
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x59,MTPSETTING);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1e,0x05);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1b,B1BH);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x57,B57H);
		sc7660_i2c_client->addr=0x1c;
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x59,MTPSETTING);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1e,0x05);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1b,B1BH);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x57,B57H);
		sc7660_i2c_client->addr=0x1d;
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x59,MTPSETTING);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1e,0x05);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1b,B1BH);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x57,B57H);
		sc7660_i2c_client->addr=0x1e;
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x59,MTPSETTING);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1e,0x05);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1b,B1BH);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x57,B57H);
		sc7660_i2c_client->addr=0x1f;
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x59,MTPSETTING);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1e,0x05);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1b,B1BH);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x57,B57H);
		}
		sc7660_i2c_client->addr=0x1d;
		msleep(100);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x24,0x80);
		i2c_smbus_write_byte_data(sc7660_i2c_client,0x1e,0x05);
		val_reg = i2c_smbus_read_byte_data(sc7660_i2c_client,0x0f);
		if(val_reg != 0x11)
			printk("momkey sc7660 resume fail! regchip_id=0x%x\n", val_reg);
	}

	result = i2c_smbus_write_byte_data(sc7660_i2c_client, sc7660_REG_CTRL, sc7660_CTRL_PWRON);
	assert(result==0);
#ifdef GSENSOR_REG_CALI
	{
		int i = 0;
		i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1E, 0x05);
		for(i = 0; i < sizeof(reg)/sizeof(reg[0]); i++) {
			if(reg_data[i] != i2c_smbus_read_byte_data(sc7660_i2c_client, reg[i])){
				i2c_smbus_write_byte_data(sc7660_i2c_client, reg[i], reg_data[i]);
			}
		}
	}
#endif
	return;
}
#else
#ifdef CONFIG_PM
static int sc7660_resume(struct i2c_client *client)
{
	return 0;
}

static int sc7660_suspend(struct i2c_client *client, pm_message_t mesg)
{
	return 0;
}
#endif
#endif /* CONFIG_HAS_EARLYSUSPEND */

static void sc7660_reset(void)
{
	int result;
	int val_reg;
	int MTPSETTING, B57H, B1BH, i;
	printk("sc7660 reset power down\n");
	result = i2c_smbus_write_byte_data(sc7660_i2c_client, sc7660_REG_CTRL, sc7660_CTRL_PWRDN);
	assert(result == 0);
	msleep(50);

	printk("sc7660 reset power on\n");
    val_reg = i2c_smbus_read_byte_data(sc7660_i2c_client, 0x0f);
	if (val_reg != 0x11) {
		for (i = 0; i < 3; i++) {
			MTPSETTING = 0x07;
			B57H = 0x00;
			B1BH = 0x08;
			sc7660_i2c_client->addr = 0x18;
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x59, MTPSETTING);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1e, 0x05);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1b, B1BH);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x57, B57H);
			sc7660_i2c_client->addr = 0x19;
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x59, MTPSETTING);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1e, 0x05);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1b, B1BH);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x57, B57H);
			sc7660_i2c_client->addr = 0x1a;
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x59, MTPSETTING);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1e, 0x05);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1b, B1BH);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x57, B57H);
			sc7660_i2c_client->addr = 0x1b;
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x59, MTPSETTING);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1e, 0x05);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1b, B1BH);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x57, B57H);
			sc7660_i2c_client->addr = 0x1c;
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x59, MTPSETTING);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1e, 0x05);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1b, B1BH);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x57, B57H);
			sc7660_i2c_client->addr = 0x1d;
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x59, MTPSETTING);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1e, 0x05);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1b, B1BH);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x57, B57H);
			sc7660_i2c_client->addr = 0x1e;
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x59, MTPSETTING);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1e, 0x05);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1b, B1BH);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x57, B57H);
			sc7660_i2c_client->addr = 0x1f;
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x59, MTPSETTING);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1e, 0x05);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1b, B1BH);
			i2c_smbus_write_byte_data(sc7660_i2c_client, 0x57, B57H);
		}
		sc7660_i2c_client->addr = 0x1d;
		msleep(100);
		i2c_smbus_write_byte_data(sc7660_i2c_client, 0x24, 0x80);
		i2c_smbus_write_byte_data(sc7660_i2c_client, 0x1e, 0x05);
		val_reg = i2c_smbus_read_byte_data(sc7660_i2c_client, 0x0f);
		if (val_reg != 0x11)
			printk("momkey sc7660 resume fail! regchip_id=0x%x\n", val_reg);
	}

	result = i2c_smbus_write_byte_data(sc7660_i2c_client, sc7660_REG_CTRL, sc7660_CTRL_PWRON);
	assert(result == 0);
}

static const struct i2c_device_id sc7660_id[] = {
	{ sc7660_DRV_NAME, 1 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sc7660_id);

static struct i2c_driver sc7660_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		.name	= sc7660_DRV_NAME,
		.owner	= THIS_MODULE,
	},
	.probe	= sc7660_probe,
	.remove	= sc7660_remove,
#ifdef CONFIG_HAS_EARLYSUSPEND
#else
#ifdef CONFIG_PM
	.suspend = sc7660_suspend,
	.resume = sc7660_resume,
#endif
#endif
	.id_table = sc7660_id,
	.detect = sc7660_gsensor_detect,
	.address_list	= normal_i2c,
};

static int __init sc7660_init(void)
{
	int ret = -1;

	printk("function=%s=========LINE=%d. \n", __func__,__LINE__);

	if(input_sensor_startup(&(gsensor_info.input_type))){
		printk("%s: err.\n", __func__);
		return -1;
	} else
	ret = input_sensor_init(&(gsensor_info.input_type));

    if (0 != ret){
        printk("%s:gsensor.init_platform_resource err. \n", __func__);
    }

	twi_id = gsensor_info.twi_id;
    input_set_power_enable(&(gsensor_info.input_type),1);

	ret = i2c_add_driver(&sc7660_driver);
	if (ret < 0) {
		printk( "add sc7660 i2c driver failed\n");
		return -ENODEV;
	}

	return ret;
}

static void __exit sc7660_exit(void)
{
	printk("remove sc7660 i2c driver.\n");
	i2c_del_driver(&sc7660_driver);
	input_sensor_free(&(gsensor_info.input_type));
}

MODULE_AUTHOR("djq");
MODULE_DESCRIPTION("sc7660 Sensor driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.1");

module_init(sc7660_init);
module_exit(sc7660_exit);

