/*
 * Copyright (c) 1996, Sujal M. Patel
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	$FreeBSD$
 *      from: pnp.c,v 1.11 1999/05/06 22:11:19 peter Exp
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <isa/isavar.h>
#include <isa/pnpreg.h>
#include <isa/pnpvar.h>
#include <machine/resource.h>
#include <machine/clock.h>

typedef struct _pnp_id {
	u_int32_t vendor_id;
	u_int32_t serial;
	u_char checksum;
} pnp_id;

struct pnp_set_config_arg {
	int	csn;		/* Card number to configure */
	int	ldn;		/* Logical device on card */
};

struct pnp_quirk {
	u_int32_t vendor_id;	/* Vendor of the card */
	u_int32_t logical_id;	/* ID of the device with quirk */
	int	type;
#define PNP_QUIRK_WRITE_REG	1 /* Need to write a pnp register  */
	int	arg1;
	int	arg2;
};

struct pnp_quirk pnp_quirks[] = {
	/*
	 * The Gravis UltraSound needs register 0xf2 to be set to 0xff
	 * to enable power.
	 * XXX need to know the logical device id.
	 */
	{ 0x0100561e /* GRV0001 */,	0,
	  PNP_QUIRK_WRITE_REG,	0xf2,	 0xff },

	{ 0 }
};

#if 0
/*
 * these entries are initialized using the autoconfig menu
 * The struct is invalid (and must be initialized) if the first
 * CSN is zero. The init code fills invalid entries with CSN 255
 * which is not a supported value.
 */

struct pnp_cinfo pnp_ldn_overrides[MAX_PNP_LDN] = {
    { 0 }
};
#endif

/* The READ_DATA port that we are using currently */
static int pnp_rd_port;

static void   pnp_send_initiation_key(void);
static int    pnp_get_serial(pnp_id *p);
static int    pnp_isolation_protocol(device_t parent);

static void
pnp_write(int d, u_char r)
{
	outb (_PNP_ADDRESS, d);
	outb (_PNP_WRITE_DATA, r);
}

#if 0

static u_char
pnp_read(int d)
{
	outb (_PNP_ADDRESS, d);
	return (inb(3 | (pnp_rd_port <<2)));
}

#endif

/*
 * Send Initiation LFSR as described in "Plug and Play ISA Specification",
 * Intel May 94.
 */
static void
pnp_send_initiation_key()
{
	int cur, i;

	/* Reset the LSFR */
	outb(_PNP_ADDRESS, 0);
	outb(_PNP_ADDRESS, 0); /* yes, we do need it twice! */

	cur = 0x6a;
	outb(_PNP_ADDRESS, cur);

	for (i = 1; i < 32; i++) {
		cur = (cur >> 1) | (((cur ^ (cur >> 1)) << 7) & 0xff);
		outb(_PNP_ADDRESS, cur);
	}
}


/*
 * Get the device's serial number.  Returns 1 if the serial is valid.
 */
static int
pnp_get_serial(pnp_id *p)
{
	int i, bit, valid = 0, sum = 0x6a;
	u_char *data = (u_char *)p;

	bzero(data, sizeof(char) * 9);
	outb(_PNP_ADDRESS, PNP_SERIAL_ISOLATION);
	for (i = 0; i < 72; i++) {
		bit = inb((pnp_rd_port << 2) | 0x3) == 0x55;
		DELAY(250);	/* Delay 250 usec */

		/* Can't Short Circuit the next evaluation, so 'and' is last */
		bit = (inb((pnp_rd_port << 2) | 0x3) == 0xaa) && bit;
		DELAY(250);	/* Delay 250 usec */

		valid = valid || bit;

		if (i < 64)
			sum = (sum >> 1) |
				(((sum ^ (sum >> 1) ^ bit) << 7) & 0xff);

		data[i / 8] = (data[i / 8] >> 1) | (bit ? 0x80 : 0);
	}

	valid = valid && (data[8] == sum);

	return valid;
}

/*
 * Fill's the buffer with resource info from the device.
 * Returns 0 if the device fails to report
 */
static int
pnp_get_resource_info(u_char *buffer, int len)
{
	int i, j;
	u_char temp;

	for (i = 0; i < len; i++) {
		outb(_PNP_ADDRESS, PNP_STATUS);
		for (j = 0; j < 100; j++) {
			if ((inb((pnp_rd_port << 2) | 0x3)) & 0x1)
				break;
			DELAY(1);
		}
		if (j == 100) {
			printf("PnP device failed to report resource data\n");
			return 0;
		}
		outb(_PNP_ADDRESS, PNP_RESOURCE_DATA);
		temp = inb((pnp_rd_port << 2) | 0x3);
		if (buffer != NULL)
			buffer[i] = temp;
	}
	return 1;
}

#if 0
/*
 * write_pnp_parms initializes a logical device with the parms
 * in d, and then activates the board if the last parameter is 1.
 */

static int
write_pnp_parms(struct pnp_cinfo *d, pnp_id *p, int ldn)
{
    int i, empty = -1 ;

    pnp_write (SET_LDN, ldn );
    i = pnp_read(SET_LDN) ;
    if (i != ldn) {
	printf("Warning: LDN %d does not exist\n", ldn);
    }
    for (i = 0; i < 8; i++) {
	pnp_write(IO_CONFIG_BASE + i * 2, d->ic_port[i] >> 8 );
	pnp_write(IO_CONFIG_BASE + i * 2 + 1, d->ic_port[i] & 0xff );
    }
    for (i = 0; i < 4; i++) {
	pnp_write(MEM_CONFIG + i*8, (d->ic_mem[i].base >> 16) & 0xff );
	pnp_write(MEM_CONFIG + i*8+1, (d->ic_mem[i].base >> 8) & 0xff );
	pnp_write(MEM_CONFIG + i*8+2, d->ic_mem[i].control & 0xff );
	pnp_write(MEM_CONFIG + i*8+3, (d->ic_mem[i].range >> 16) & 0xff );
	pnp_write(MEM_CONFIG + i*8+4, (d->ic_mem[i].range >> 8) & 0xff );
    }
    for (i = 0; i < 2; i++) {
	pnp_write(IRQ_CONFIG + i*2    , d->irq[i] );
	pnp_write(IRQ_CONFIG + i*2 + 1, d->irq_type[i] );
	pnp_write(DRQ_CONFIG + i, d->drq[i] );
    }
    /*
     * store parameters read into the current kernel
     * so manual editing next time is easier
     */
    for (i = 0 ; i < MAX_PNP_LDN; i++) {
	if (pnp_ldn_overrides[i].csn == d->csn &&
		pnp_ldn_overrides[i].ldn == ldn) {
	    d->flags = pnp_ldn_overrides[i].flags ;
	    pnp_ldn_overrides[i] = *d ;
	    break ;
	} else if (pnp_ldn_overrides[i].csn < 1 ||
		pnp_ldn_overrides[i].csn == 255)
	    empty = i ;
    }
    if (i== MAX_PNP_LDN && empty != -1)
	pnp_ldn_overrides[empty] = *d;

    /*
     * Here should really perform the range check, and
     * return a failure if not successful.
     */
    pnp_write (IO_RANGE_CHECK, 0);
    DELAY(1000); /* XXX is it really necessary ? */
    pnp_write (ACTIVATE, d->enable ? 1 : 0);
    DELAY(1000); /* XXX is it really necessary ? */
    return 1 ;
}
#endif

/*
 * This function is called after the bus has assigned resource
 * locations for a logical device.
 */
static void
pnp_set_config(void *arg, struct isa_config *config, int enable)
{
	int csn = ((struct pnp_set_config_arg *) arg)->csn;
	int ldn = ((struct pnp_set_config_arg *) arg)->ldn;
	int i;

	/*
	 * First put all cards into Sleep state with the initiation
	 * key, then put our card into Config state.
	 */
	pnp_send_initiation_key();
	pnp_write(PNP_WAKE, csn);

	/*
	 * Select our logical device so that we can program it.
	 */
	pnp_write(PNP_SET_LDN, ldn);

	/*
	 * Now program the resources.
	 */
	for (i = 0; i < config->ic_nmem; i++) {
		u_int32_t start = config->ic_mem[i].ir_start;
		u_int32_t size =  config->ic_mem[i].ir_size;
		if (start & 0xff)
			panic("pnp_set_config: bogus memory assignment");
		pnp_write(PNP_MEM_BASE_HIGH(i), (start >> 16) & 0xff);
		pnp_write(PNP_MEM_BASE_LOW(i), (start >> 8) & 0xff);
		pnp_write(PNP_MEM_RANGE_HIGH(i), (size >> 16) & 0xff);
		pnp_write(PNP_MEM_RANGE_LOW(i), (size >> 8) & 0xff);
	}
	for (; i < ISA_NMEM; i++) {
		pnp_write(PNP_MEM_BASE_HIGH(i), 0);
		pnp_write(PNP_MEM_BASE_LOW(i), 0);
		pnp_write(PNP_MEM_RANGE_HIGH(i), 0);
		pnp_write(PNP_MEM_RANGE_LOW(i), 0);
	}

	for (i = 0; i < config->ic_nport; i++) {
		u_int32_t start = config->ic_port[i].ir_start;
		pnp_write(PNP_IO_BASE_HIGH(i), (start >> 8) & 0xff);
		pnp_write(PNP_IO_BASE_LOW(i), (start >> 0) & 0xff);
	}
	for (; i < ISA_NPORT; i++) {
		pnp_write(PNP_IO_BASE_HIGH(i), 0);
		pnp_write(PNP_IO_BASE_LOW(i), 0);
	}

	for (i = 0; i < config->ic_nirq; i++) {
		int irq = ffs(config->ic_irqmask[i]) - 1;
		pnp_write(PNP_IRQ_LEVEL(i), irq);
		pnp_write(PNP_IRQ_TYPE(i), 2); /* XXX */
	}
	for (; i < ISA_NIRQ; i++) {
		/*
		 * IRQ 0 is not a valid interrupt selection and
		 * represents no interrupt selection.
		 */
		pnp_write(PNP_IRQ_LEVEL(i), 0);
	}		

	for (i = 0; i < config->ic_ndrq; i++) {
		int drq = ffs(config->ic_drqmask[i]) - 1;
		pnp_write(PNP_DMA_CHANNEL(i), drq);
	}
	for (; i < ISA_NDRQ; i++) {
		/*
		 * DMA channel 4, the cascade channel is used to
		 * indicate no DMA channel is active.
		 */
		pnp_write(PNP_DMA_CHANNEL(i), 4);
	}		

	pnp_write(PNP_ACTIVATE, enable ? 1 : 0);

	/*
	 * Wake everyone up again, we are finished.
	 */
	pnp_write(PNP_CONFIG_CONTROL, PNP_CONFIG_CONTROL_WAIT_FOR_KEY);
}

/*
 * Process quirks for a logical device.. The card must be in Config state.
 */
static void
pnp_check_quirks(u_int32_t vendor_id, u_int32_t logical_id, int ldn)
{
	struct pnp_quirk *qp;

	for (qp = &pnp_quirks[0]; qp->vendor_id; qp++) {
		if (qp->vendor_id == vendor_id
		    && (qp->logical_id == 0
			|| qp->logical_id == logical_id)) {
			switch (qp->type) {
			case PNP_QUIRK_WRITE_REG:
				pnp_write(PNP_SET_LDN, ldn);
				pnp_write(qp->arg1, qp->arg2);
				break;
			}
		}
	}
}

/*
 * Scan Resource Data for Logical Devices.
 *
 * This function exits as soon as it gets an error reading *ANY*
 * Resource Data or ir reaches the end of Resource Data.  In the first
 * case the return value will be TRUE, FALSE otherwise.
 */
static int
pnp_scan_resdata(device_t parent, pnp_id *p, int csn)
{
	u_char tag, resinfo[16];
	int large_len, scanning = 1024, retval = FALSE;
	u_int32_t logical_id;
	u_int32_t compat_id;
	device_t dev = 0;
	int ldn = 0;
	struct isa_config card, logdev, alt;
	struct isa_config *config;
	struct pnp_set_config_arg *csnldn;
	int priority = 0;
	char *desc = 0;

	bzero(&card, sizeof card);
	bzero(&logdev, sizeof logdev);
	bzero(&alt, sizeof alt);
	config = &card;
	while (scanning-- > 0 && pnp_get_resource_info(&tag, 1)) {
		if (PNP_RES_TYPE(tag) == 0) {
			/* Small resource */
			if (pnp_get_resource_info(resinfo,
						  PNP_SRES_LEN(tag)) == 0) {
				scanning = 0;
				continue;
			}

			switch (PNP_SRES_NUM(tag)) {
			case PNP_TAG_LOGICAL_DEVICE:
				/* 
				 * A new logical device. Scan
				 * resourcea and add device.
				 */
				bcopy(resinfo, &logical_id, 4);
				pnp_check_quirks(p->vendor_id,
						 logical_id,
						 ldn);
				compat_id = 0;
				logdev = card;
				config = &logdev;
				dev = BUS_ADD_CHILD(parent,
						    ISA_ORDER_PNP, NULL, -1);
				if (desc)
					device_set_desc_copy(dev, desc);
				isa_set_vendorid(dev, p->vendor_id);
				isa_set_serial(dev, p->serial);
				isa_set_logicalid(dev, logical_id);
				csnldn = malloc(sizeof *csnldn,
						M_DEVBUF, M_NOWAIT);
				if (!csnldn) {
					device_printf(parent,
						      "out of memory\n");
					scanning = 0;
					break;
				}
				csnldn->csn = csn;
				csnldn->ldn = ldn;
				ISA_SET_CONFIG_CALLBACK(parent, dev,
							pnp_set_config,
							csnldn);
				ldn++;
				break;
		    
			case PNP_TAG_COMPAT_DEVICE:
				/*
				 * Got a compatible device id
				 * resource. Should keep a list of
				 * compat ids in the device.
				 */
				bcopy(resinfo, &compat_id, 4);
				if (dev)
					isa_set_compatid(dev, compat_id);
				break;
		    
			case PNP_TAG_IRQ_FORMAT:
				if (config->ic_nirq == ISA_NIRQ) {
					device_printf(parent,
						      "CSN %d too many irqs",
						      csn);
					scanning = 0;
					break;
				}
				config->ic_irqmask[config->ic_nirq] =
					resinfo[0] + (resinfo[1]<<8);
				config->ic_nirq++;
				break;

			case PNP_TAG_DMA_FORMAT:
				if (config->ic_ndrq == ISA_NDRQ) {
					device_printf(parent,
						      "CSN %d too many drqs",
						      csn);
					scanning = 0;
					break;
				}
				config->ic_drqmask[config->ic_ndrq] =
					resinfo[0];
				config->ic_ndrq++;
				break;

			case PNP_TAG_START_DEPENDANT:
				if (config == &alt) {
					ISA_ADD_CONFIG(parent, dev,
						       priority, config);
				} else if (config != &logdev) {
					device_printf(parent,
						      "CSN %d malformed\n",
						      csn);
					scanning = 0;
					break;
				}
				/*
				 * If the priority is not specified,
				 * then use the default of
				 * 'acceptable'
				 */
				if (PNP_SRES_LEN(tag) > 0)
					priority = resinfo[0];
				else
					priority = 1;
				alt = logdev;
				config = &alt;
				break;

			case PNP_TAG_END_DEPENDANT:
				ISA_ADD_CONFIG(parent, dev, priority, config);
				config = &logdev;
				break;

			case PNP_TAG_IO_RANGE:
				if (config->ic_nport == ISA_NPORT) {
					device_printf(parent,
						      "CSN %d too many ports",
						      csn);
					scanning = 0;
					break;
				}
				config->ic_port[config->ic_nport].ir_start =
					resinfo[1] + (resinfo[2]<<8);
				config->ic_port[config->ic_nport].ir_end =
					resinfo[3] + (resinfo[4]<<8)
					+ resinfo[6] - 1;
				config->ic_port[config->ic_nport].ir_size
					=
					resinfo[6];
				config->ic_port[config->ic_nport].ir_align =
					resinfo[5];
				config->ic_nport++;
				break;

			case PNP_TAG_IO_FIXED:
				if (config->ic_nport == ISA_NPORT) {
					device_printf(parent,
						      "CSN %d too many ports",
						      csn);
					scanning = 0;
					break;
				}
				config->ic_port[config->ic_nport].ir_start =
					resinfo[0] + (resinfo[1]<<8);
				config->ic_port[config->ic_nport].ir_end =
					resinfo[0] + (resinfo[1]<<8)
					+ resinfo[2] - 1;
				config->ic_port[config->ic_nport].ir_size
					= resinfo[2];
				config->ic_port[config->ic_nport].ir_align = 1;
				config->ic_nport++;
				break;

			case PNP_TAG_END:
				scanning = 0;
				break;

			default:
				/* Skip this resource */
				break;
			}
		} else {
			/* Large resource */
			if (pnp_get_resource_info(resinfo, 2) == 0) {
				scanning = 0;
				continue;
			}
			large_len = resinfo[0] + (resinfo[1] << 8);

			if (PNP_LRES_NUM(tag) == PNP_TAG_ID_ANSI) {
				if (desc)
					free(desc, M_TEMP);
				desc = malloc(large_len + 1,
					      M_TEMP, M_NOWAIT);
				/*
				 * Note: if malloc fails, this will
				 * skip the resource instead of
				 * reading it into desc.
				 */
				if (pnp_get_resource_info(desc,
							  large_len) == 0) {
					scanning = 0;
				}
				if (desc) {
					/*
					 * Trim trailing spaces.
					 */
					while (desc[large_len-1] == ' ')
						large_len--;
					desc[large_len] = '\0';
					if (dev)
						device_set_desc_copy
							(dev, desc);
				}
				continue;
			}

			if (PNP_LRES_NUM(tag) != PNP_TAG_MEMORY_RANGE) {
				/* skip */
				if (pnp_get_resource_info(NULL,
							  large_len) == 0) {
					scanning = 0;
				}
				continue;
			}

			if (pnp_get_resource_info(resinfo, large_len) == 0) {
				scanning = 0;
				continue;
			}

			if (config->ic_nmem == ISA_NMEM) {
				device_printf(parent,
					      "CSN %d too many memory ranges",
					      csn);
				scanning = 0;
				break;
			}

			config->ic_mem[config->ic_nmem].ir_start =
				(resinfo[4]<<8) + (resinfo[5]<<16);
			config->ic_mem[config->ic_nmem].ir_end =
				(resinfo[6]<<8) + (resinfo[7]<<16);
			config->ic_mem[config->ic_nmem].ir_size =
				(resinfo[10]<<8) + (resinfo[11]<<16);
			config->ic_mem[config->ic_nmem].ir_align =
				resinfo[8] + (resinfo[9]<<8);
			if (!config->ic_mem[config->ic_nmem].ir_align)
				config->ic_mem[config->ic_nmem].ir_align =
					0x10000;
			config->ic_nmem++;
		}
	}

	if (desc)
		free(desc, M_TEMP);

	return retval;
}

/*
 * Run the isolation protocol. Use pnp_rd_port as the READ_DATA port
 * value (caller should try multiple READ_DATA locations before giving
 * up). Upon exiting, all cards are aware that they should use
 * pnp_rd_port as the READ_DATA port.
 *
 * In the first pass, a csn is assigned to each board and pnp_id's
 * are saved to an array, pnp_devices. In the second pass, each
 * card is woken up and the device configuration is called.
 */
static int
pnp_isolation_protocol(device_t parent)
{
	int csn;
	pnp_id id;
	int found = 0;

	/*
	 * Put all cards into the Sleep state so that we can clear
	 * their CSNs.
	 */
	pnp_send_initiation_key();

	/*
	 * Clear the CSN for all cards.
	 */
	pnp_write(PNP_CONFIG_CONTROL, PNP_CONFIG_CONTROL_RESET_CSN);

	/*
	 * Move all cards to the Isolation state.
	 */
	pnp_write(PNP_WAKE, 0);

	/*
	 * Tell them where the read point is going to be this time.
	 */
	pnp_write(PNP_SET_RD_DATA, pnp_rd_port);

	for (csn = 1; csn < PNP_MAX_CARDS; csn++) {
		/*
		 * Start the serial isolation protocol.
		 */
		outb(_PNP_ADDRESS, PNP_SERIAL_ISOLATION);
		DELAY(1000);	/* Delay 1 msec */

		if (pnp_get_serial(&id)) {
			/*
			 * We have read the id from a card
			 * successfully. The card which won the
			 * isolation protocol will be in Isolation
			 * mode and all others will be in Sleep.  *
			 * Program the CSN of the isolated card
			 * (taking it to Config state) and read its
			 * resources, creating devices as we find
			 * logical devices on the card.
			 */
			pnp_write(PNP_SET_CSN, csn);
			pnp_scan_resdata(parent, &id, csn);
			found++;
		} else
			break;

		/*
		 * Put this card back to the Sleep state and
		 * simultaneously move all cards which don't have a
		 * CSN yet to Isolation state.
		 */
		pnp_write(PNP_WAKE, 0);
	}

	/*
	 * Unless we have chosen the wrong read port, all cards will
	 * be in Sleep state. Put them back into WaitForKey for
	 * now. Their resources will be programmed later.
	 */
	pnp_write(PNP_CONFIG_CONTROL, PNP_CONFIG_CONTROL_WAIT_FOR_KEY);

	return found;
}


/*
 * pnp_identify()
 *
 * autoconfiguration of pnp devices. This routine just runs the
 * isolation protocol over several ports, until one is successful.
 *
 * may be called more than once ?
 *
 */

static void
pnp_identify(driver_t *driver, device_t parent)
{
	int num_pnp_devs;

#if 0
	if (pnp_ldn_overrides[0].csn == 0) {
		if (bootverbose)
			printf("Initializing PnP override table\n");
		bzero (pnp_ldn_overrides, sizeof(pnp_ldn_overrides));
		pnp_ldn_overrides[0].csn = 255 ;
	}
#endif

	/* Try various READ_DATA ports from 0x203-0x3ff */
	for (pnp_rd_port = 0x80; (pnp_rd_port < 0xff); pnp_rd_port += 0x10) {
		if (bootverbose)
			printf("Trying Read_Port at %x\n", (pnp_rd_port << 2) | 0x3);

		num_pnp_devs = pnp_isolation_protocol(parent);
		if (num_pnp_devs)
			break;
	}
}

static device_method_t pnp_methods[] = {
	/* Device interface */
	DEVMETHOD(device_identify,	pnp_identify),

	{ 0, 0 }
};

static driver_t pnp_driver = {
	"pnp",
	pnp_methods,
	1,			/* no softc */
};

static devclass_t pnp_devclass;

DRIVER_MODULE(pnp, isa, pnp_driver, pnp_devclass, 0, 0);
