#ifndef FBS_GPIO_H

#define FBS_GPIO_H

#define MAX_GPIO_BANKS              (4)

// GPIO register size
#define AM335X_GPIO_SIZE		    0x1000

#define AM335X_GPIO_OE		    	0x134
#define AM335X_GPIO_DATAIN			0x138
#define AM335X_GPIO_DATAOUT			0x13C
#define AM335X_GPIO_SETDATAOUT		0x190
#define AM335X_GPIO_CLEARDATAOUT	0x194

#define OUT          (0)
#define IN           (1)

extern unsigned int gpio_mirror[];
extern volatile unsigned int *gpio_datain_addr[];


void gpio_set_direction(int bank, int pin, int direction);
void gpio_set(int bank, int pin);
void gpio_clear(int bank, int pin);
unsigned int gpio_read_bank(int bank);
int gpio_read_pin(int bank, int pin);
void gpio_write_bank_from_mirror(int bank);
void gpio_init();

#endif