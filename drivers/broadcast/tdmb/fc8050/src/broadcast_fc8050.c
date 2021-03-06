#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
//#include <linux/i2c.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <linux/interrupt.h>

#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/wakelock.h> 		/* wake_lock, unlock */

#include "../../broadcast_tdmb_drv_ifdef.h"
#include "../inc/broadcast_fc8050.h"
#include "../inc/fci_types.h"
#include "../inc/bbm.h"

//#include <linux/pm_qos.h> // FEATURE_DMB_USE_PM_QOS

#include <linux/err.h>
#include <mach/msm_xo.h>

//#include CONFIG_BOARD_HEADER_FILE
#include "../../../../../arch/arm/mach-msm/lge/mako/board-mako.h"
/* external function */
extern int broadcast_drv_if_isr(void);
extern void fc8050_isr_control(fci_u8 onoff);

/* proto type declare */
static int broadcast_tdmb_fc8050_probe(struct spi_device *spi);
static int broadcast_tdmb_fc8050_remove(struct spi_device *spi);
static int broadcast_tdmb_fc8050_suspend(struct spi_device *spi, pm_message_t mesg);
static int broadcast_tdmb_fc8050_resume(struct spi_device *spi);

/* GK & J1 APQ8064 GPIOs */
#define DMB_EN					85
#define DMB_INT_N				77
#define DMB_RESET_N				1

/* SPI Data read using workqueue */
//#define FEATURE_DMB_USE_WORKQUEUE
#define FEATURE_DMB_USE_XO
//#define FEATURE_DMB_USE_PM_QOS

/************************************************************************/
/* LINUX Driver Setting                                                 */
/************************************************************************/
static uint32 user_stop_flg = 0;
struct tdmb_fc8050_ctrl_blk
{
	boolean									TdmbPowerOnState;
	struct spi_device*						spi_ptr;
#ifdef FEATURE_DMB_USE_WORKQUEUE
	struct work_struct						spi_work;
	struct workqueue_struct*				spi_wq;
#endif
	struct mutex							mutex;
	struct wake_lock						wake_lock;	/* wake_lock,wake_unlock */
	boolean									spi_irq_status;
	spinlock_t								spin_lock;
#ifdef FEATURE_DMB_USE_XO
	struct msm_xo_voter						*xo_handle_tdmb;
#endif
#ifdef FEATURE_DMB_USE_PM_QOS
	struct pm_qos_request    pm_req_list;
#endif
};

static struct tdmb_fc8050_ctrl_blk fc8050_ctrl_info;

#if 0
void tdmb_fc8050_spi_write_read_test(void)
{
	uint16 i;
	uint32 wdata = 0;
	uint32 ldata = 0;
	uint32 data = 0;
	uint32 temp = 0;

#define TEST_CNT    1
	tdmb_fc8050_power_on();

	for(i=0;i<TEST_CNT;i++)
	{
		BBM_WRITE(NULL, 0x05, i & 0xff);
		BBM_READ(NULL, 0x05, (fci_u8*)&data);
		if((i & 0xff) != data)
#ifdef CONFIG_FC8050_DEBUG
			printk("FC8000 byte test (0x%x,0x%x)\n", i & 0xff, data);
#endif
	}

	for(i=0;i<TEST_CNT;i++)
	{
		BBM_WORD_WRITE(NULL, 0x0210, i & 0xffff);
		BBM_WORD_READ(NULL, 0x0210, (fci_u16*)&wdata);
		if((i & 0xffff) != wdata)
#ifdef CONFIG_FC8050_DEBUG
			printk("FC8000 word test (0x%x,0x%x)\n", i & 0xffff, wdata);
#endif
	}

	for(i=0;i<TEST_CNT;i++)
	{
		BBM_LONG_WRITE(NULL, 0x0210, i & 0xffffffff);
		BBM_LONG_READ(NULL, 0x0210, (fci_u32*)&ldata);
		if((i & 0xffffffff) != ldata)
#ifdef CONFIG_FC8050_DEBUG
			printk("FC8000 long test (0x%x,0x%x)\n", i & 0xffffffff, ldata);
#endif
	}

	data = 0;

	for(i=0;i<TEST_CNT;i++)
	{
		temp = i&0xff;
		BBM_TUNER_WRITE(NULL, 0x12, 0x01, (fci_u8*)&temp, 0x01);
		BBM_TUNER_READ(NULL, 0x12, 0x01, (fci_u8*)&data, 0x01);
		if((i & 0xff) != data)
#ifdef CONFIG_FC8050_DEBUG
			printk("FC8000 tuner test (0x%x,0x%x)\n", i & 0xff, data);
#endif
	}
	temp = 0x51;
	BBM_TUNER_WRITE(NULL, 0x12, 0x01, (fci_u8*)&temp, 0x01 );
	tdmb_fc8050_power_off();
}
#endif

struct spi_device *tdmb_fc8050_get_spi_device(void)
{
	return fc8050_ctrl_info.spi_ptr;
}


void tdmb_fc8050_set_userstop(int mode)
{
	user_stop_flg = mode;
}

int tdmb_fc8050_mdelay(int32 ms)
{
	int32	wait_loop =0;
	int32	wait_ms = ms;
	int		rc = 1;  /* 0 : false, 1 : ture */

	if(ms > 100)
	{
		wait_loop = (ms /100);   /* 100, 200, 300 more only , Otherwise this must be modified e.g (ms + 40)/50 */
		wait_ms = 100;
	}

	do
	{
		mdelay(wait_ms);
		if(user_stop_flg == 1)
		{
#ifdef CONFIG_FC8050_DEBUG
			printk("~~~~~~~~ Ustop flag is set so return false ms =(%d)~~~~~~~\n", ms);
#endif
			rc = 0;
			break;
		}
	}while((--wait_loop) > 0);

	return rc;
}

void tdmb_fc8050_Must_mdelay(int32 ms)
{
	mdelay(ms);
}

int tdmb_fc8050_tdmb_is_on(void)
{
	return (int)fc8050_ctrl_info.TdmbPowerOnState;
}

/* EXPORT_SYMBOL() : when we use external symbol
which is not included in current module - over kernel 2.6 */
//EXPORT_SYMBOL(tdmb_fc8050_tdmb_is_on);


int tdmb_fc8050_power_on(void)
{
#ifdef FEATURE_DMB_USE_XO
	int rc;
#endif

#ifdef CONFIG_FC8050_DEBUG
	printk("tdmb_fc8050_power_on \n");
#endif
	if ( fc8050_ctrl_info.TdmbPowerOnState == FALSE )
	{
#ifdef FEATURE_DMB_USE_XO
		rc = msm_xo_mode_vote(fc8050_ctrl_info.xo_handle_tdmb, MSM_XO_MODE_ON);
		if(rc < 0) {
			pr_err("Configuring MSM_XO_MODE_ON failed (%d)\n", rc);
			msm_xo_put(fc8050_ctrl_info.xo_handle_tdmb);
			return FALSE;
		}
#endif
#ifdef FEATURE_DMB_USE_PM_QOS
		if(pm_qos_request_active(&fc8050_ctrl_info.pm_req_list)) {
			pm_qos_update_request(&fc8050_ctrl_info.pm_req_list, 20);
		}
#endif

		wake_lock(&fc8050_ctrl_info.wake_lock);

		gpio_set_value(DMB_EN, 0);
		gpio_set_value(DMB_RESET_N, 1);
		udelay(100);

		gpio_set_value(DMB_EN, 1);
		udelay(1000); /* Due to X-tal stablization Time */

		gpio_set_value(DMB_RESET_N, 0);
		udelay(100);
		gpio_set_value(DMB_RESET_N, 1);

		tdmb_fc8050_interrupt_free();
		fc8050_ctrl_info.TdmbPowerOnState = TRUE;

#ifdef CONFIG_FC8050_DEBUG
		printk("tdmb_fc8050_power_on OK\n");
#endif
	}
#ifdef CONFIG_FC8050_DEBUG
	else
	{
		printk("tdmb_fc8050_power_on the power already turn on \n");
	}

	printk("tdmb_fc8050_power_on completed \n");
#endif

	return TRUE;
}

int tdmb_fc8050_power_off(void)
{
	if ( fc8050_ctrl_info.TdmbPowerOnState == TRUE )
	{
		tdmb_fc8050_interrupt_lock();

#ifdef FEATURE_DMB_USE_XO
		if(fc8050_ctrl_info.xo_handle_tdmb != NULL) {
		    msm_xo_mode_vote(fc8050_ctrl_info.xo_handle_tdmb, MSM_XO_MODE_OFF);
		}
#endif

		fc8050_ctrl_info.TdmbPowerOnState = FALSE;

		gpio_set_value(DMB_RESET_N, 0);
		gpio_set_value(DMB_EN, 0);

		wake_unlock(&fc8050_ctrl_info.wake_lock);
#ifdef FEATURE_DMB_USE_PM_QOS
		if(pm_qos_request_active(&fc8050_ctrl_info.pm_req_list)) {
			pm_qos_update_request(&fc8050_ctrl_info.pm_req_list, PM_QOS_DEFAULT_VALUE);	
		}
#endif
	}
#ifdef CONFIG_FC8050_DEBUG
	else
	{
		printk("tdmb_fc8050_power_on the power already turn off \n");
	}

	printk("tdmb_fc8050_power_off completed \n");
#endif

	return TRUE;
}

int tdmb_fc8050_select_antenna(unsigned int sel)
{
	return FALSE;
}

static struct spi_driver broadcast_tdmb_driver = {
	.probe = broadcast_tdmb_fc8050_probe,
	.remove	= __devexit_p(broadcast_tdmb_fc8050_remove),
	.suspend = broadcast_tdmb_fc8050_suspend,
	.resume  = broadcast_tdmb_fc8050_resume,
	.driver = {
		.name = "tdmb_fc8050",
		.bus	= &spi_bus_type,
		.owner = THIS_MODULE,
	},
};

void tdmb_fc8050_interrupt_lock(void)
{
#ifdef CONFIG_FC8050_DEBUG
	if (fc8050_ctrl_info.spi_ptr == NULL)
	{
		printk("tdmb_fc8050_interrupt_lock fail\n");
	}
	else
	{
		disable_irq(fc8050_ctrl_info.spi_ptr->irq);
	}
#else
	if (fc8050_ctrl_info.spi_ptr != NULL)
	{
		disable_irq(fc8050_ctrl_info.spi_ptr->irq);
	}
#endif
}

void tdmb_fc8050_interrupt_free(void)
{
#ifdef CONFIG_FC8050_DEBUG
	if (fc8050_ctrl_info.spi_ptr == NULL)
	{
		printk("tdmb_fc8050_interrupt_free fail\n");
	}
	else
	{
		enable_irq(fc8050_ctrl_info.spi_ptr->irq);
	}
#else
	if (fc8050_ctrl_info.spi_ptr != NULL)
	{
		enable_irq(fc8050_ctrl_info.spi_ptr->irq);
	}
#endif
}

int tdmb_fc8050_spi_write_read(uint8* tx_data, int tx_length, uint8 *rx_data, int rx_length)
{
	int rc;

	struct spi_transfer	t = {
			.tx_buf		= tx_data,
			.rx_buf		= rx_data,
			.len		= tx_length+rx_length,
		};

	struct spi_message	m;

	if (fc8050_ctrl_info.spi_ptr == NULL)
	{
#ifdef CONFIG_FC8050_DEBUG
		printk("tdmb_fc8050_spi_write_read error txdata=0x%x, length=%d\n", (unsigned int)tx_data, tx_length+rx_length);
#endif
		return FALSE;
	}

	mutex_lock(&fc8050_ctrl_info.mutex);

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	rc = spi_sync(fc8050_ctrl_info.spi_ptr, &m);

#ifdef CONFIG_FC8050_DEBUG
	if ( rc < 0 )
	{
		printk("tdmb_fc8050_spi_read_burst result(%d), actual_len=%d\n",rc, m.actual_length);
	}
#endif

	mutex_unlock(&fc8050_ctrl_info.mutex);

	return TRUE;
}

#ifdef FEATURE_DMB_USE_WORKQUEUE
static irqreturn_t broadcast_tdmb_spi_isr(int irq, void *handle)
{
	struct tdmb_fc8050_ctrl_blk* fc8050_info_p;
	unsigned long flag;

	fc8050_info_p = (struct tdmb_fc8050_ctrl_blk *)handle;
	if ( fc8050_info_p && fc8050_info_p->TdmbPowerOnState )
	{
		if (fc8050_info_p->spi_irq_status)
		{
#ifdef CONFIG_FC8050_DEBUG
			printk("######### spi read function is so late skip #########\n");			
#endif
			return IRQ_HANDLED;
		}
//		printk("***** broadcast_tdmb_spi_isr coming *******\n");
		spin_lock_irqsave(&fc8050_info_p->spin_lock, flag);
		queue_work(fc8050_info_p->spi_wq, &fc8050_info_p->spi_work);
		spin_unlock_irqrestore(&fc8050_info_p->spin_lock, flag);
	}
#ifdef CONFIG_FC8050_DEBUG
	else
	{
		printk("broadcast_tdmb_spi_isr is called, but device is off state\n");
	}
#endif

	return IRQ_HANDLED;
}

static void broacast_tdmb_spi_work(struct work_struct *tdmb_work)
{
	struct tdmb_fc8050_ctrl_blk *pTdmbWorkData;

	pTdmbWorkData = container_of(tdmb_work, struct tdmb_fc8050_ctrl_blk, spi_work);
	if ( pTdmbWorkData )
	{
		fc8050_isr_control(0);
		pTdmbWorkData->spi_irq_status = TRUE;
		broadcast_drv_if_isr();
		pTdmbWorkData->spi_irq_status = FALSE;
		fc8050_isr_control(1);
	}
#ifdef CONFIG_FC8050_DEBUG
	else
	{
		printk("~~~~~~~broadcast_tdmb_spi_work call but pTdmbworkData is NULL ~~~~~~~\n");
	}
#endif
}
#else
static irqreturn_t broadcast_tdmb_spi_event_handler(int irq, void *handle)
{
	struct tdmb_fc8050_ctrl_blk* fc8050_info_p;

	fc8050_info_p = (struct tdmb_fc8050_ctrl_blk *)handle;
	if ( fc8050_info_p && fc8050_info_p->TdmbPowerOnState )
	{
		if (fc8050_info_p->spi_irq_status)
		{
#ifdef CONFIG_FC8050_DEBUG
			printk("######### spi read function is so late skip ignore #########\n");
#endif
			return IRQ_HANDLED;
		}

		fc8050_isr_control(0);
		fc8050_info_p->spi_irq_status = TRUE;
		broadcast_drv_if_isr();
		fc8050_info_p->spi_irq_status = FALSE;
		fc8050_isr_control(1);
	}
#ifdef CONFIG_FC8050_DEBUG
	else
	{
		printk("broadcast_tdmb_spi_isr is called, but device is off state\n");
	}
#endif

	return IRQ_HANDLED;
}
#endif

static int tdmb_configure_gpios(void)
{
	int rc = OK;
	int err_count = 0;

	rc = gpio_request(DMB_RESET_N, "DMB_RESET_N");
	if (rc < 0) {
		err_count++;
		printk("%s:Failed GPIO DMB_RESET_N request!!!\n",__func__);
	}

	rc = gpio_request(DMB_EN, "DMB_EN");
	if (rc < 0) {
		err_count++;
		printk("%s:Failed GPIO DMB_EN request!!!\n",__func__);
	}

	rc = gpio_request(DMB_INT_N, "DMB_INT_N");
	if (rc < 0) {
		err_count++;
		printk("%s:Failed GPIO DMB_INT_N request!!!\n",__func__);
	}

	gpio_direction_output(DMB_RESET_N, 0);
	gpio_direction_output(DMB_EN, 0);
	gpio_direction_input(DMB_INT_N);

	if(err_count > 0) rc = -EINVAL;

	return rc;
}

static int broadcast_tdmb_fc8050_probe(struct spi_device *spi)
{
	int rc;

	if(spi == NULL)
	{
#ifdef CONFIG_FC8050_DEBUG
		printk("broadcast_fc8050_probe spi is NULL, so spi can not be set\n");
#endif
		return -1;
	}

	fc8050_ctrl_info.TdmbPowerOnState = FALSE;

	fc8050_ctrl_info.spi_ptr 				= spi;
	fc8050_ctrl_info.spi_ptr->mode 			= SPI_MODE_0;
	fc8050_ctrl_info.spi_ptr->bits_per_word 	= 8;
	fc8050_ctrl_info.spi_ptr->max_speed_hz 	= (24000*1000);
	rc = spi_setup(spi);
#ifdef CONFIG_FC8050_DEBUG
	printk("broadcast_tdmb_fc8050_probe spi_setup=%d\n", rc);
#endif
	BBM_HOSTIF_SELECT(NULL, 1);

#ifdef FEATURE_DMB_USE_XO
    fc8050_ctrl_info.xo_handle_tdmb = msm_xo_get(MSM_XO_TCXO_A2, "TDMB");
	if(IS_ERR(fc8050_ctrl_info.xo_handle_tdmb)) {
		pr_err("Failed to get MSM_XO_TCXO_A2 handle for TDMB (%ld)\n", PTR_ERR(fc8050_ctrl_info.xo_handle_tdmb));
		return FALSE;
	}
#endif

#ifdef FEATURE_DMB_USE_WORKQUEUE
	INIT_WORK(&fc8050_ctrl_info.spi_work, broacast_tdmb_spi_work);
	fc8050_ctrl_info.spi_wq = create_singlethread_workqueue("tdmb_spi_wq");
	if(fc8050_ctrl_info.spi_wq == NULL){
		printk("Failed to setup tdmb spi workqueue \n");
		return -ENOMEM;
	}
#endif

	tdmb_configure_gpios( );

#ifdef FEATURE_DMB_USE_WORKQUEUE
	rc = request_irq(spi->irq, broadcast_tdmb_spi_isr, IRQF_DISABLED | IRQF_TRIGGER_FALLING, 
	                   spi->dev.driver->name, &fc8050_ctrl_info);
#else
	rc = request_threaded_irq(spi->irq, NULL, broadcast_tdmb_spi_event_handler, IRQF_DISABLED | IRQF_TRIGGER_FALLING,
	                   spi->dev.driver->name, &fc8050_ctrl_info);
#endif
#ifdef CONFIG_FC8050_DEBUG
	printk("broadcast_tdmb_fc8050_probe request_irq=%d\n", rc);
#endif

	tdmb_fc8050_interrupt_lock();

	mutex_init(&fc8050_ctrl_info.mutex);

	wake_lock_init(&fc8050_ctrl_info.wake_lock,  WAKE_LOCK_SUSPEND, dev_name(&spi->dev));

	spin_lock_init(&fc8050_ctrl_info.spin_lock);
#ifdef FEATURE_DMB_USE_PM_QOS
	pm_qos_add_request(&fc8050_ctrl_info.pm_req_list, PM_QOS_CPU_DMA_LATENCY, PM_QOS_DEFAULT_VALUE);
#endif
#ifdef CONFIG_FC8050_DEBUG
	printk("broadcast_fc8050_probe End\n");
#endif

	return rc;
}

static int broadcast_tdmb_fc8050_remove(struct spi_device *spi)
{
#ifdef CONFIG_FC8050_DEBUG
	printk("broadcast_tdmb_fc8050_remove \n");
#endif

#ifdef FEATURE_DMB_USE_WORKQUEUE
	if (fc8050_ctrl_info.spi_wq)
	{
		flush_workqueue(fc8050_ctrl_info.spi_wq);
		destroy_workqueue(fc8050_ctrl_info.spi_wq);
	}
#endif

	free_irq(spi->irq, &fc8050_ctrl_info);

	mutex_destroy(&fc8050_ctrl_info.mutex);

	wake_lock_destroy(&fc8050_ctrl_info.wake_lock);
#ifdef FEATURE_DMB_USE_PM_QOS
	pm_qos_remove_request(&fc8050_ctrl_info.pm_req_list);
#endif
	memset((unsigned char*)&fc8050_ctrl_info, 0x0, sizeof(struct tdmb_fc8050_ctrl_blk));
	return 0;
}

static int broadcast_tdmb_fc8050_suspend(struct spi_device *spi, pm_message_t mesg)
{
#ifdef CONFIG_FC8050_DEBUG
	printk("broadcast_tdmb_fc8050_suspend \n");
#endif
	return 0;
}

static int broadcast_tdmb_fc8050_resume(struct spi_device *spi)
{
#ifdef CONFIG_FC8050_DEBUG
	printk("broadcast_tdmb_fc8050_resume \n");
#endif
	return 0;
}

int __devinit broadcast_tdmb_drv_init(void)
{
	int rc;

	rc = broadcast_tdmb_drv_start();
	printk("broadcast_tdmb_fc8050_probe start %d\n", rc);

	return spi_register_driver(&broadcast_tdmb_driver);
}

static void __exit broadcast_tdmb_drv_exit(void)
{
	spi_unregister_driver(&broadcast_tdmb_driver);
}



module_init(broadcast_tdmb_drv_init);
module_exit(broadcast_tdmb_drv_exit);

/* optional part when we include driver code to build-on
it's just used when we make device driver to module(.ko)
so it doesn't work in build-on */
MODULE_DESCRIPTION("FC8050 tdmb device driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("FCI");

