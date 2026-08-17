#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/printk.h>
//led0 pwm

static const struct pwm_dt_spec pwm_led0 = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));//pwm device instance

#define Minimopwmperiodo PWM_SEC(1U) / 128U//define Maximopwmperiodo PWM_SEC(1U) / 128U = 7812500 nanoseconds
#define Maximopwmperiodo PWM_SEC(1U)//define Maximopwmperiodo PWM
void main(void)
{
    uint32_t maxp;
    uint32_t p;
    uint8_t dir = 0U;//direction variable
    int ret;

    printk("PWM-based blinky\n");
    
}
