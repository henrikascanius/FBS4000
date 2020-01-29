// FBS4000 - Future Backing Storage for RC4000 w. DRC401 and a BeagleBone (tm)
// (c) 2019 by Henrik Ascanius Jacobsen, Dansk Datahistorisk Forening


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "fbs_log.h"

// #define OVERCLOCK 1
// #define STANDALONE_TEST 1  // For timing tests on unconnected BB

// ****************************
// ***** FBS GPIO INPUTS: *****
// ****************************

// Input, serial output from DSA shift register
#define GP_SRDATA_BANK      0
#define GP_SRDATA_BIT       20

// Input, Write Data from DRC
#define GP_WRDATA_BANK      1   // SP0(0)
#define GP_WRDATA_BIT       12

// Input, Write Enable from DRC
#define GP_WE_BANK          1
#define GP_WE_BIT           14

// Input, CP_DSP, clock pulse incrementing Drum Segment Address in DRC
#define GP_CPDSA_BANK       1
#define GP_CPDSA_BIT        16

// Input, latched G_i_First_Segment - First Segment was written to DRC by RC4000
#define GP_SRRQ_BANK        1
#define GP_SRRQ_BIT         17


// *****************************
// ***** FBS GPIO OUTPUTS: *****
// *****************************

// Output, clock to DSA shift register
#define GP_SRCLK_BANK       0
#define GP_SRCLK_BIT        26

// Output, Connected status to DRC
#define GP_CONN_BANK        1
#define GP_CONN_BIT         29

// Output, drum index to DRC
#define GP_INDEX_BANK       2
#define GP_INDEX_BIT        1

// Output, Read Data to DRC
#define GP_RDDATA_BANK      2
#define GP_RDDATA_BIT       3

// Output, drum clock to DRC
#define GP_RDCLK_BANK       2
#define GP_RDCLK_BIT        4

// LED outputs:

#define GP_LED0_BANK        3
#define GP_LED0_BIT         19

#define GP_LED1_BANK        3
#define GP_LED1_BIT         21

#define GP_LED2_BANK        2
#define GP_LED2_BIT         24

#define GP_LED3_BANK        2
#define GP_LED3_BIT         25

#define GP_LED4_BANK        2
#define GP_LED4_BIT         13

#define GP_LED5_BANK        2
#define GP_LED5_BIT         11

#define GP_LED6_BANK        1
#define GP_LED6_BIT         28

#define GP_LED7_BANK        2
#define GP_LED7_BIT         8

int led_banks[] = {GP_LED0_BANK, GP_LED1_BANK, GP_LED2_BANK, GP_LED3_BANK,
                   GP_LED4_BANK, GP_LED5_BANK, GP_LED6_BANK, GP_LED7_BANK};
                   
int led_bits[] =  {GP_LED0_BIT, GP_LED1_BIT, GP_LED2_BIT, GP_LED3_BIT,
                   GP_LED4_BIT, GP_LED5_BIT, GP_LED6_BIT, GP_LED7_BIT};                    

#define MAX_GPIO_BANKS              (4)

// GPIO register size
#define AM335X_GPIO_SIZE		    0x1000

#define AM335X_GPIO_OE		    	0x134
#define AM335X_GPIO_DATAIN			0x138
#define AM335X_GPIO_DATAOUT			0x13C
#define AM335X_GPIO_CLEARDATAOUT	0x190
#define AM335X_GPIO_SETDATAOUT		0x194

#define OUT          (0)
#define IN           (1)

#define CONFIG_MODULE_BASE          0x44E10000

uint32_t gpio_base[MAX_GPIO_BANKS] = {
	0x44E07000,
	0x4804C000,
	0x481AC000,
	0x481AE000
};

void *gpio_addr[MAX_GPIO_BANKS] = { NULL };

uint32_t *gpio_oe_addr[MAX_GPIO_BANKS]                  = { NULL };
volatile uint32_t *gpio_dataout_addr[MAX_GPIO_BANKS]    = { NULL };
volatile uint32_t *gpio_datain_addr[MAX_GPIO_BANKS]     = { NULL };
uint32_t *gpio_setdataout_addr[MAX_GPIO_BANKS]          = { NULL };
uint32_t *gpio_cleardataout_addr[MAX_GPIO_BANKS]        = { NULL };

uint32_t gpio_mirror[MAX_GPIO_BANKS];

#define INVMASK  0x66666600
#define MAXUNITS 4

int unit_fd[MAXUNITS];
static uint32_t *img[MAXUNITS];
uint32_t unit_segs[MAXUNITS];
int seek_error = 0;
int dirty[4];
int disconnected = 0;
uint32_t trackcnt = 0;

int unit_to_led[MAXUNITS] = {7, 6, 5, 4};

#define ACC_LED         3
#define WR_LED          2
#define ERR_LED         1
#define ERR_LATCH_LED   0

static uint32_t ledoff_at[8];  // track count for led off

int selected_unit = -1;
uint32_t dsa = 0;  // Drum Segment Address

// Track buffer, 4*268 24-bit words
uint32_t trbuf[4*268];

void abend(char *s)
{
    FBS_LOG(G_ERROR, "ABEND: %s", s); 
    fprintf(stderr, "ABEND: %s\n", s);
    exit(1);
}

void gpio_set_direction(int bank, int pin, int direction)
{
	uint32_t reg;
	reg = *gpio_oe_addr[bank];
	if (direction == OUT) {
		reg = reg & ~(1 << pin);
	} else {
		reg = reg | (1 << pin);
	}
    *gpio_oe_addr[bank] = reg;
}

void gpio_set(int bank, int pin)
{   // Not faster than write the whole mirror...
	*gpio_setdataout_addr[bank] = (1 << pin);
	gpio_mirror[bank] |= (1 << pin);
}

void gpio_clear(int bank, int pin)
{
	*gpio_cleardataout_addr[bank] = (1 << pin);
	gpio_mirror[bank] &= (~(1 << pin));
}

uint32_t gpio_read_bank(int bank)
{
    return *gpio_datain_addr[bank];
}

int gpio_read_pin(int bank, int pin)
{
    return *gpio_datain_addr[bank] & (1<<pin) ? 1:0;
}


void gpio_write_bank_from_mirror(int bank)
{
    *gpio_dataout_addr[bank] = gpio_mirror[bank];
}

// Update GPIO bank 2 w. clk, data, index bits
#define UPD_DRC *gpio_dataout_addr[2] = gpio_mirror[2]

void set_led(int led, int val)
{   // LED is on when GPIO out is low
    if (val)
        gpio_mirror[led_banks[led]] &= ~(1 << led_bits[led]);
    else
        gpio_mirror[led_banks[led]] |= 1 << led_bits[led];
        gpio_write_bank_from_mirror(led_banks[led]);
}

void flash_led(int led)
{
    set_led(led, 1);
#ifndef OVERCLOCK
    ledoff_at[led] = trackcnt + 6;
#else
    ledoff_at[led] = trackcnt + 10;
#endif
}

void upd_leds()
{
    for (int led=0; led<8; led++)
    {
        if (ledoff_at[led] == trackcnt)
            set_led(led, 0);
    }
}

void gpio_init()
{
    int mem_fd;
	int bank = 0, i;
	uint32_t *config;
	
	/* Setup pinmux for pins that are not GPIO already */
	
	if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
		fprintf(stderr, "main: can't open /dev/mem \n");
		exit(-1);
	}
	config = mmap(  NULL,                   //Any adddress in our space will do
                    0x2000,                 //Map length
                    PROT_READ|PROT_WRITE,   // Enable reading & writting to mapped memory
                    MAP_SHARED,             //Shared with other processes
                    mem_fd,                 //File to map
                    CONFIG_MODULE_BASE      //Offset to GPIO peripheral
                  );
    
    if (config[0x8E8>>2] != 0x00000027 ||
        config[0x8EC>>2] != 0x00000027 ||
        config[0x8BC>>2] != 0x00000027 ||
        config[0x8B4>>2] != 0x00000027 ||
        config[0x8A8>>2] != 0x00000027)
    {
        fprintf(stderr, "PINMUX settings detected: \n");
        fprintf(stderr, "GPIO88: %08X\n",config[0x8E8>>2]);
        fprintf(stderr, "GPIO89: %08X\n",config[0x8EC>>2]);
        fprintf(stderr, "GPIO77: %08X\n",config[0x8BC>>2]);
        fprintf(stderr, "GPIO75: %08X\n",config[0x8B4>>2]);
        fprintf(stderr, "GPIO72: %08X\n",config[0x8A8>>2]);
        fprintf(stderr, "One or more required GPIO pins have incorrect pinmux settings\n");
        fprintf(stderr, "Please include disable_uboot_overlay_video=1 in /boot/uEnv.txt\n");
        exit(1);
    }

    /* map access to GPIO banks */
    
	for (bank=0; bank<MAX_GPIO_BANKS; bank++)
	{
        gpio_addr[bank] = mmap( NULL,                   //Any adddress in our space will do
                                AM335X_GPIO_SIZE,       //Map length
                                PROT_READ|PROT_WRITE,   // Enable reading & writting to mapped memory
                                MAP_SHARED,             //Shared with other processes
                                mem_fd,                 //File to map
                                gpio_base[bank]         //Offset to GPIO peripheral
                              );
        
        gpio_oe_addr[bank] = gpio_addr[bank] + AM335X_GPIO_OE;             
        gpio_datain_addr[bank] = gpio_addr[bank] + AM335X_GPIO_DATAIN;             
        gpio_dataout_addr[bank] = gpio_addr[bank] + AM335X_GPIO_DATAOUT;             
        gpio_setdataout_addr[bank] = gpio_addr[bank] + AM335X_GPIO_SETDATAOUT;             
        gpio_cleardataout_addr[bank] = gpio_addr[bank] + AM335X_GPIO_CLEARDATAOUT;             
    } 
    
	close(mem_fd); //No need to keep mem_fd open after mmap
	for (bank = 0; bank < MAX_GPIO_BANKS; bank++) {
		if (gpio_addr[bank] == MAP_FAILED) {
			fprintf(stderr, "mmap error %d\n", (int)gpio_addr[bank]);//errno also set!
			exit(-1);
		}
	}
    
	gpio_set_direction(GP_SRDATA_BANK, GP_SRDATA_BIT, IN);
	gpio_set_direction(GP_WRDATA_BANK, GP_WRDATA_BIT, IN);
	gpio_set_direction(GP_WE_BANK, GP_WE_BIT, IN);
	gpio_set_direction(GP_CPDSA_BANK, GP_CPDSA_BIT, IN);
	gpio_set_direction(GP_SRRQ_BANK, GP_SRRQ_BIT, IN);

    gpio_set_direction(GP_SRCLK_BANK, GP_SRCLK_BIT, OUT);
    gpio_set_direction(GP_CONN_BANK, GP_CONN_BIT, OUT);
    gpio_set_direction(GP_INDEX_BANK, GP_INDEX_BIT, OUT);
    gpio_set_direction(GP_RDDATA_BANK, GP_RDDATA_BIT, OUT);
    gpio_set_direction(GP_RDCLK_BANK, GP_RDCLK_BIT, OUT);

	for (i=0; i<8; i++)
	    gpio_set_direction(led_banks[i], led_bits[i], OUT);
	
	for (bank = 0; bank < MAX_GPIO_BANKS; bank++)
		gpio_mirror[bank] = *gpio_dataout_addr[bank];
		
    gpio_clear(GP_SRCLK_BANK, GP_SRCLK_BIT);
    gpio_clear(GP_CONN_BANK, GP_CONN_BIT);
    gpio_set(GP_INDEX_BANK, GP_INDEX_BIT);
    gpio_clear(GP_RDDATA_BANK, GP_RDDATA_BIT);
    gpio_clear(GP_RDCLK_BANK, GP_RDCLK_BIT);
} // gpio_init

int cmd(char *cmd)
{   // Execute a shell command
	int pstatus;
	FBS_LOG(G_MISC, "Execute command: %s", cmd);
	pid_t pid = fork();
	if (pid==-1) abend("fork, cmd");
	if (pid != 0) {	waitpid(pid, &pstatus, 0); return pstatus; }
	execlp("sh", "sh", "-c", cmd, NULL);
	abend("exec, cmd");
}

void file_init()
{
    char uname[6];
    char *fname;
    struct stat sb;
    int units = 0;
    char *startcmd;
    
    // Intended to e.g. set fs in RW mode
    startcmd = getenv("FBS_START");
    if (startcmd)
        cmd(startcmd);
    
    strcpy(uname,"UNIT");
    uname[5] = 0;
    
    for (int unit=0; unit<MAXUNITS; unit++)
    {
        uname[4] = unit + '0';
        fname = getenv(uname);
        if (fname)
        {
            // unit_fd not used at present, fd could be closed after each mmap
            if ((unit_fd[unit] = open(uname, O_RDWR|O_SYNC)) < 0)
            {
                fprintf(stderr, "File not found: %s\n", fname);
                exit(1);
            }
            if (fstat(unit_fd[unit], &sb)== -1)
                abend("fstat");
            if (sb.st_size < 768*4) // At least one track...
                abend("filesize");
            img[unit] = mmap(NULL, sb.st_size, PROT_READ|PROT_WRITE,
                             MAP_PRIVATE | MAP_POPULATE,
                             unit_fd[unit], 0);
            if (img[unit] == MAP_FAILED)
                abend("mmap");
            unit_segs[unit] = sb.st_size / 768;
            units++;
        }
        else
        {
            unit_fd[unit] = -1;
            img[unit] = NULL;
            unit_segs[unit] = 0;
        }
    }
    if (!units)
        abend("No disk units");
} // file_init

void file_close()
{
    char *stopcmd;
    
    for (int unit=0; unit<MAXUNITS; unit++)
    {
        if (img[unit])
        {
            munmap(img[unit], unit_segs[unit]*768);
            close(unit_fd[unit]);
            img[unit] = NULL;
            unit_segs[unit] = 0;
            unit_fd[unit] = -1;
        }
    }
    
   // Intended to e.g. set fs in RO mode
    stopcmd = getenv("FBS_STOP");
    if (stopcmd) cmd(stopcmd);
}

int poll_dsa(uint32_t *reg)
{
    int i;
    uint32_t sr = 0;
    
    if (!gpio_read_pin(GP_SRRQ_BANK, GP_SRRQ_BIT))
        return 0;
    
    // SRCLK is inverted by IC4C
    for (i=0; i<19; i++)
    {
        gpio_set(GP_SRCLK_BANK, GP_SRCLK_BIT); // CP 1->0
        sr = (sr >> 1) | (gpio_read_pin(GP_SRDATA_BANK, GP_SRDATA_BIT) << 18);
        gpio_clear(GP_SRCLK_BANK, GP_SRCLK_BIT); // CP 0->1
    }
    *reg = sr;
    return 1;
}

uint32_t elapsed_us(struct timeval tv, struct timeval tbase)
{
    return (tv.tv_sec - tbase.tv_sec)*1000000 + tv.tv_usec - tbase.tv_usec;
}

void set_connected(int conn)
{
    if (conn)
        gpio_set(GP_CONN_BANK, GP_CONN_BIT);
    else
        gpio_clear(GP_CONN_BANK, GP_CONN_BIT);
}

void fetch_track()
{
    // Build track image from file data
    uint32_t track = dsa >> 2;
    uint32_t *imgptr;
    int tridx = 0;
    uint32_t parity;
    
    seek_error = (track << 2) > unit_segs[selected_unit];
    if (!seek_error)
    {
        imgptr = img[selected_unit] + track*768; // (768 b / 4b/w) * 4 seg/tr
        for (int sect=0; sect<4; sect++)
        {
            parity = (((track<<2) + sect) << 8) | 0x80000000;
            // We keep the word numbering of the DRC...
            // Sector data occupies word 0..255. Reformat to 24-bit
            //    24-bit:     32-bit (file):
            //      cba0            dcba                           
            //      fed0            hgfe
            //      ihg0            lkji
            //      lkj0
            for (int i=0; i<256/4; i++)
            {
                parity ^= (trbuf[tridx++] = (imgptr[0] << 8) ^ INVMASK);
                parity ^= (trbuf[tridx++] = ((((imgptr[0] & 0xff000000) >> 16) | (imgptr[1] << 16))) ^INVMASK);
                parity ^= (trbuf[tridx++] = ((((imgptr[1] & 0xffff0000) >> 8)  | (imgptr[2] << 24))) ^INVMASK);
                parity ^= (trbuf[tridx++] = (imgptr[2] & 0xffffff00) ^INVMASK);
                imgptr += 3;
            }
            trbuf[tridx++] = parity;
            for (int i=0; i<11; i++)
            {
                trbuf[tridx++] = (((track << 2) + ((sect+1)&3))<< 8) | 0x80000000; // See DRC018
            }
        }
        bzero(dirty, sizeof(dirty));
    }
    else
    {
        // seek error, make sync. error on DRC
        bzero(trbuf, sizeof(trbuf));
    }
}

void select_unit(int unit)
{
    if (unit == selected_unit)
        return;
    
    if (unit < 0 || unit >= MAXUNIT)
    {
        FBS_LOG(G_ERROR, "*** OUT OF RANGE Unit select: %d", unit);
        abend("select_unit");
    }

    if (selected_unit >= 0)
        set_led(unit_to_led[selected_unit], 0);
    selected_unit = unit;
    if (img[unit])
    {
        fetch_track();
        set_led(unit_to_led[unit], 1);
        FBS_LOG(G_SEEK, "Unit select: %d", unit);
        set_connected(1);
        disconnected = 0;
    }
    else
    {
        FBS_LOG(G_SEEK, "OFFLINE Unit select: %d", unit);
        set_connected(0);
        disconnected = 1;
    }
}

void wait_powerok()
{
    // The cpdsa signal from DRC is forced to 1 when power_ok is false (25V off)
    int cpdsa;
    int ledon = 0;
    
    while (1)
    {
        set_led(ERR_LATCH_LED, (ledon = !ledon));
        cpdsa = 0;
        for (int j=0; j<24; j++)  // Could be just 2...
        {
            gpio_mirror[2] = gpio_mirror[2] | (1<<GP_RDCLK_BIT);
            UPD_DRC;
            gpio_mirror[2] = gpio_mirror[2] & ~(1<<GP_RDCLK_BIT);
            UPD_DRC;
            UPD_DRC;  // For correct timing
            cpdsa += (*gpio_datain_addr[GP_CPDSA_BANK] & (1<<GP_CPDSA_BIT)) != 0;
        }
        if (cpdsa < 2)
        {
            FBS_LOG(G_MISC, "DRC POWER ON(%d)", cpdsa);
            set_led(ERR_LATCH_LED, 0);
            return;
        }
        else
        {
            sleep(1);
        }
    }
}

        
void flush_track()
{
    // Update dirty sectors in file data
    uint32_t track = dsa >> 2;
    uint32_t *imgptr;
    int tridx;

    for (int sect=0; sect<4; sect++)
    {
        if (dirty[sect])
        {
            imgptr = img[selected_unit] + track*768 + sect*(768/4);
            tridx = sect*268;
            for (int i=0; i<256/4; i++)
            {
                *(imgptr++) = (trbuf[tridx] >> 8) | ((trbuf[tridx+1] & 0x0000ff00) << 16);
                *(imgptr++) = (trbuf[tridx+1] >> 16) | ((trbuf[tridx+2] & 0x00ffff00) << 8);
                *(imgptr++) = (trbuf[tridx+2] >> 24) | (trbuf[tridx+3] & 0xffffff00);
                tridx += 4;
            }
            dirty[sect] = 0;
        }
    }
}

int send_rcv_words(uint32_t *ptr, int words, uint32_t *wbuf)
{
    // Common RD/WR loop. Writedata collected in wbuf, calc. parity appened.
    // wbuf must hold (words+1) words 
    int32_t w;
    int wr_ena;
    uint32_t gpb1;
    uint32_t parity = 0;
    uint32_t wr_word = 0;
    
    for (int i=0; i<words; i++)
    {
        w = (int32_t)(*(ptr++));
        for (int j=0; j<24; j++)
        {
            // Get writedata, in case it's write...
            gpb1 = *gpio_datain_addr[GP_WRDATA_BANK];
            wr_word = (wr_word<<1) | ((gpb1 & (1<<GP_WRDATA_BIT)) != 0);
            
            // Data is sampled 200 ns after pos edge on clk-GPIO. 
            // Output sigs are inverted by 74LS02
            gpio_mirror[2] = (gpio_mirror[2] & ~(1<<GP_RDDATA_BIT)) |
                             (1<<GP_RDCLK_BIT) |
                             ((w>=0) << GP_RDDATA_BIT);
            UPD_DRC;
            gpio_mirror[2] &= ~(1<<GP_RDCLK_BIT);
            UPD_DRC;
#ifndef OVERCLOCK
            UPD_DRC;  // For correct timing
#endif
            w += w;
        }
        if (i < words-1)
            // Store wrdate, calc parity
            parity ^= (*(wbuf++) = (wr_word << 8) ^ INVMASK);
        else
            // Save received parity or address word, no inversions
            *(wbuf++) = (wr_word << 8); 
    }
    *wbuf = parity; // append calculated parity (w.o. segm addr word)
    return (gpb1 & (1<<GP_WE_BIT)) == 0;  // WE in same bank as WR_DATA, WE is inverted at 68A1
}

int do_word_257_267(uint32_t *ptr, int index_sector, uint32_t *w267)
{
    // Send address words from ptr
    // Handle track change; return 1 if WE
    // Collect address word (w267) from DRC, for write check
    int32_t w;
    int cpdsa = 0;
    int chtrack = 0;
    int chunit = 0;
    uint32_t newunit;
    uint32_t newdsa;
    uint32_t nonsense[11];
    int wr_ena;
    
    // Handle Word257:
    w = (int32_t)(*(ptr++));
    for (int j=0; j<24; j++)
    {
        // Data is sampled 200 ns after pos edge on clk-GPIO. 
        // All sigs are inverted by 74LS02
        gpio_mirror[2] &= ~(1<<GP_RDDATA_BIT);
        gpio_mirror[2] = gpio_mirror[2] | (1<<GP_RDCLK_BIT) |((w>=0) << GP_RDDATA_BIT);
        if (index_sector && (j==3))
            gpio_mirror[2] &= ~(1<<GP_INDEX_BIT);
        else
            gpio_mirror[2] |= 1<<GP_INDEX_BIT;
        UPD_DRC;
        gpio_mirror[2] = gpio_mirror[2] & ~(1<<GP_RDCLK_BIT);
        UPD_DRC;
#ifndef OVERCLOCK
        UPD_DRC;  // For correct timing
#endif
        cpdsa += (*gpio_datain_addr[GP_CPDSA_BANK] & (1<<GP_CPDSA_BIT)) != 0;
        w += w;
    }
    
    // Handle segment# update
    if (cpdsa > 1)
    {
        FBS_LOG(G_MISC, "DRC POWER FAULT(%d)", cpdsa);
        return -1;
    }
    
    if (poll_dsa(&newdsa))
    {   // DSA was written by RC4000
        newunit = (newdsa >> 17) & 3;
        chunit = (newunit != selected_unit);
        newdsa &= 0x1ffff;
        chtrack = chunit || ((newdsa & 0x1fffc) != (dsa & 0x1fffc));
        FBS_LOG(G_SEEK, "New Unit, DSA: %d %d", newunit, newdsa);
        flash_led(ACC_LED);
    }
    else
    if (cpdsa==1)
    {   // Track boundary crossed by R/W
        newdsa = ((dsa+1) & 0x1FFFF);
        chtrack = (newdsa & 3) == 0;
        FBS_LOG(G_SEEK, "Incr DSA: %d", newdsa);
    }
    else
        newdsa = dsa;
    
    if (chtrack)
    {
        flush_track();
        if (chunit)
            select_unit(newunit);
        dsa = newdsa;
        fetch_track();  // Changes the data ptr points at!!
    }
    else
        dsa = newdsa;
    
    // end segment# update
        
    // Send 258-267
    wr_ena = send_rcv_words(ptr, 10, nonsense);
    *w267 = nonsense[9]; // Last word is not INVMASKED :)
#ifdef STANDALONE_TEST
    return 0;
#endif
    return wr_ena;
}

void main_loop()
{
    uint32_t *trp;
    uint32_t wr_buf[258];
    int wr_ena = 0;
    uint32_t w267_DRC;
    int wr_fault = 0;
    uint32_t calc_parity;
    uint32_t segm_addr_w;
    struct timeval starttime, laptime, now;
    struct timeval lap2;
    uint32_t tr_time, tmin=1000000, tmax=0;

    gettimeofday(&starttime, NULL);
    laptime = starttime;
    lap2 = laptime;
    while (1)
    {
        trp = trbuf; 
        for (int sect=0; sect<4; sect++)
        {
            send_rcv_words(trp, 257, wr_buf); // data + parity
            set_connected(!disconnected);  // Clear temp. error status
            if (wr_ena)
            {
                calc_parity = wr_buf[257] ^ segm_addr_w; 
                wr_fault |= wr_buf[256] != calc_parity;
                if (wr_fault)
                {
                    set_connected(0);  // Only means we have to signal write error
                    FBS_LOG(G_ERROR, "WRITE ERROR Segm: %d Addr: %08x Exp: %08x Parity: %08x Exp: %08x  W257: %08x",
                                     dsa, w267_DRC, segm_addr_w, wr_buf[256], calc_parity, wr_buf[257]);
                    flash_led(ERR_LED);
                    set_led(ERR_LATCH_LED, 1);
                }
                else
                {   // Write OK: Update sector in trbuf
                    memcpy(trbuf+(sect*268), wr_buf, 257*4);
                    dirty[sect] = 1;
                    FBS_LOG(G_DATA, "Write data: Sector: %d Data[0..1]: %06X %06X",
                                    (dsa & 0x1fffc)+sect,
                                    wr_buf[0] >> 8,
                                    wr_buf[1] >> 8);
                    flash_led(WR_LED);
                }
            }
            trp += 257;
            wr_ena = do_word_257_267(trp, sect==3, &w267_DRC);
            if (wr_ena < 0)
            {
                flush_track();
                return; // Power fault
            }
            
            segm_addr_w = ((((dsa & 0x1FFFC) + ((sect+1)&3)) << 8) | 0x80000000); // Address is for *next* sector on track
            wr_fault = wr_ena && ((w267_DRC != segm_addr_w) || seek_error);
            trp += 11;
        }
        trackcnt++;
        upd_leds();
        
        // Monitor min/max rotation time
        gettimeofday(&now, NULL);
        tr_time = elapsed_us(now, lap2);
        lap2 = now;
        if (tr_time > tmax)
            tmax = tr_time; 
        if (tr_time < tmin)
            tmin = tr_time;
        
        if (!(trackcnt & 127))
        {
            flush_track(); // Don't let written data get stuck in trackbuf
            if (!(trackcnt & 2047))
            {
                gettimeofday(&now, NULL);
                FBS_LOG(G_STAT, "Min/max/avg rotation time: %d/%d/%d us",
                         tmin, tmax, elapsed_us(now, laptime)/2048);
                tmin = 1000000;
                tmax = 0;
                laptime = now;
            }
        }
    }
}

        
int main (int argc, char *argv[])
{
	int i = 0, j = 0;
    int tw;
    int rw;
    int dummy;
    int first = 1;
 
    fbs_openlog();
	gpio_init();
	
	// Turn LEDs ON
    for (j=0; j<8; j++)
    {
        set_led(j,1);
    }

	// Abend immediately if file problems
	file_init();
	file_close();

	FBS_LOG(G_MISC, "Started");
	
	while (1) // loop over +25V on/off cycles
	{
        // Turn LEDs OFF
        for (j=0; j<8; j++)
        {
            set_led(j,0);
        }
    
	    wait_powerok();
	    file_init();

        // Short LED test at startup
        for (j=0; j<8; j++)
        {
            set_led(j,1);
            usleep(100000);
            set_led(j,0);
            usleep(100000);
        }

        if (first)
        {
            for (int unit=0; unit<MAXUNITS; unit++)
            {
                if (img[unit])
                {
                    select_unit(unit);
                    break;
                }
            }
            dsa = 0;
        }
        else
            select_unit(selected_unit);
        
        fetch_track();
        main_loop();
        file_close();
    }
}