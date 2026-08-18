/*
	Copyright 1999, Be Incorporated.   All Rights Reserved.
	This file may be used under the terms of the Be Sample Code License.

	Other authors:
	Mark Watson,
	Rudolf Cornelissen 10/2002-1/2016.
*/

#define MODULE_BIT 0x00800000

#include <string.h>
#include <unistd.h>
#include "acc_std.h"

static status_t init_common(int the_fd);

/* Initialization code shared between primary and cloned accelerants */
static status_t init_common(int the_fd) {
	status_t result;
	nv_get_private_data gpd;
	
	// LOG not available from here to next LOG: NULL si

	/* memorize the file descriptor */
	fd = the_fd;
	/* set the magic number so the driver knows we're for real */
	gpd.magic = NV_PRIVATE_DATA_MAGIC;
	/* contact driver and get a pointer to the registers and shared data */
	result = ioctl(fd, NV_GET_PRIVATE_DATA, &gpd, sizeof(gpd));
	if (result != B_OK)
		goto error0;

	/* clone the shared area for our use */
	shared_info_area = clone_area(DRIVER_PREFIX " shared", (void **)&si, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, gpd.shared_info_area);
	if (shared_info_area < 0) {
			result = shared_info_area;
			goto error0;
	}
	// LOG is now available, si !NULL
#ifdef __POWERPC__
	/* ppc: targeted accelerant logging to /boot/home/nvidia.*.log.
	 * 0xffffffff meant EVERY module at EVERY level, which wrote a huge log on
	 * every mode set and visibly slowed startup. The LOG macro splits the mask:
	 * the high 28 bits select MODULE_BIT, the low nibble selects the level. So
	 * this enables level 1 only, and only for the two modules carrying the ppc
	 * acceleration diagnostics:
	 *     0x00080000  engine/nv_acc.c    (ppc-acc: engine + FIFO + pixel format)
	 *     0x00200000  SetDisplayMode.c   (ppc-acc: STAGE / draw test)
	 *     0x00000001  level 1
	 * Raise to 0xffffffff temporarily when the full CRTC/DAC dumps are wanted. */
	si->settings.logmask = 0x202c0005;	/* nv_acc.c + nv_crtc.c + SetDisplayMode.c + Cursor.c, levels 1 and 4 */
	/* ppc: 2D acceleration is ENABLED. The old comment here claimed "the 2D
	 * engine is not initialized on ppc" - it is now, and it demonstrably works:
	 * a rectangle filled by the engine and a second one blitted from it were
	 * both visible on hardware. What had actually been broken was a big-endian
	 * read of the 16-bit FifoFree field, which made every FIFO wait time out
	 * (see nv_acc.h and b62b98cfed).
	 *
	 * Exporting the hooks only matters because AccelerantHWInterface now has a
	 * caller for them (HWInterface::AcceleratedBlit); before that, nothing in
	 * app_server fetched a single acceleration hook. */
	/* ppc: acceleration DISABLED for now - it works, but the engine discards
	 * the byte it treats as X padding, which in our A,R,G,B framebuffer layout
	 * is BLUE, so accelerated blits turn white windows yellow. Everything else
	 * (engine init, the big-endian FifoFree fix, AcceleratedBlit in app_server)
	 * is in place; set this to false to turn it back on once the pixel format
	 * question is solved. See the 2D notes for what has been ruled out. */
	si->settings.block_acc = false;
	/* PIO, not DMA: CHKA in GetAccelerantHook.c exports the _DMA hook variants
	 * when dma_acc is set (it defaults true), but we initialise the PIO engine
	 * via nv_acc_init(). Mismatched, the DMA blit silently does nothing. */
	si->settings.dma_acc = false;
	/* ppc: HARDWARE cursor. This was previously forced off on the grounds that
	 * the hw cursor path "is a suspected corruptor" - but that was a guess from
	 * before modesetting worked, and was never verified. With a software cursor
	 * every mouse move costs app_server a region recomposite plus a byte-swapped
	 * copy into VRAM across AGP (see HWInterface::_CopyToFront); in hardware it
	 * costs two register writes. Note nv_crtc_cursor_define() needs the ppc byte
	 * swap in nv_crtc.c to go with this - without it black cursor pixels come out
	 * as a colour. */
	si->settings.hardcursor = true;
#endif
	LOG(4,("init_common: logmask 0x%08x, memory %dMB, hardcursor %d, usebios %d, switchhead %d, force_pci %d\n",
		si->settings.logmask, si->settings.memory, si->settings.hardcursor, si->settings.usebios, si->settings.switchhead, si->settings.force_pci));
	LOG(4,("init_common: dumprom %d, unhide_fw %d, pgm_panel %d, dma_acc %d, tv_output %d, vga_on_tv %d\n",
		si->settings.dumprom, si->settings.unhide_fw, si->settings.pgm_panel, si->settings.dma_acc, si->settings.tv_output, si->settings.vga_on_tv));
	LOG(4,("init_common: force_sync %d, gpu_clk %dMhz, ram_clk %dMhz, force_ws %d, block_acc %d, check_edid %d\n",
		si->settings.force_sync, si->settings.gpu_clk, si->settings.ram_clk, si->settings.force_ws, si->settings.block_acc, si->settings.check_edid));

 	/*Check for R4.5.0 and if it is running, use work around*/
 	{
 		if (si->use_clone_bugfix)
 		{
 			/*check for R4.5.0 bug and attempt to work around*/
 			LOG(2,("InitACC: Found R4.5.0 bug - attempting to work around\n"));
 			regs = si->clone_bugfix_regs;
 		}
 		else
 		{
			/* clone the memory mapped registers for our use  - does not work on <4.5.2 (but is better this way)*/
			regs_area = clone_area(DRIVER_PREFIX " regs", (void **)&regs, B_ANY_ADDRESS,
				B_READ_AREA | B_WRITE_AREA, si->regs_area);
			if (regs_area < 0) {
				result = regs_area;
				goto error1;
			}
 		}
 	}

	/* all done */
	goto error0;

error1:
	delete_area(shared_info_area);
error0:
	return result;
}

/* Clean up code shared between primary and cloned accelrants */
static void uninit_common(void) {
	/* release the memory mapped registers */
	delete_area(regs_area);
	/* a little cheap paranoia */
	regs = 0;
	/* release our copy of the shared info from the kernel driver */
	delete_area(shared_info_area);
	/* more cheap paranoia */
	si = 0;
}

/*
Initialize the accelerant.  the_fd is the file handle of the device (in
/dev/graphics) that has been opened by the app_server (or some test harness).
We need to determine if the kernel driver and the accelerant are compatible.
If they are, get the accelerant ready to handle other hook functions and
report success or failure.
*/
status_t INIT_ACCELERANT(int the_fd)
{
	status_t result;
	int pointer_reservation; //mem reserved for pointer
	int cnt; 				 //used for iteration through the overlay buffers

	if (0) {
		time_t now = time (NULL);
		// LOG not available from here to next LOG: NULL si
		MSG(("INIT_ACCELERANT: %s", ctime (&now)));
	}

	/* note that we're the primary accelerant (accelerantIsClone is global) */
	accelerantIsClone = 0;

	/* do the initialization common to both the primary and the clones */
	result = init_common(the_fd);

	/* bail out if the common initialization failed */
	if (result != B_OK)
		goto error0;
	// LOG now available: !NULL si
	
	/* ensure that INIT_ACCELERANT is executed just once (copies should be clones) */
	if (si->accelerant_in_use)
	{
		result = B_NOT_ALLOWED;
		goto error1;
	}

#ifdef __POWERPC__
	/* ppc RUNG 2 - driving display: kernel attached + switched regs to BE.
	 * Proceed into powerup + (conservative) modeset. */
	LOG(1, ("INIT: ppc rung 2 - driving display\n"));
#endif

	/* call the device specific init code */
	result = nv_general_powerup();

	/* bail out if it failed */
	if (result != B_OK)
		goto error1;

	/*
	Now would be a good time to figure out what video modes your card supports.
	We'll place the list of modes in another shared area so all of the copies
	of the driver can see them.  The primary copy of the accelerant (ie the one
	initialized with this routine) will own the "one true copy" of the list.
	Everybody else get's a read-only clone.
	*/
	result = create_mode_list();
	if (result != B_OK)
		goto error1;

	/*
	Put the cursor at the start of the frame buffer.
	Nvidia cursor is 32x32 16 color? takes up 4096 bytes of RAM.
	*/
	/* Initialize the rest of the cursor information while we're here */
	si->cursor.width = 16;
	si->cursor.height = 16;
	si->cursor.hot_x = 0;
	si->cursor.hot_y = 0;
	si->cursor.x = 0;
	si->cursor.y = 0;
	si->cursor.dh_right = false;

	/*
	Put the frame buffer immediately following the cursor data. We store this
	info in a frame_buffer_config structure to make it convienient to return
	to the app_server later.
	*/
	pointer_reservation = 0;
	/* Nvidia hardcursor: reserve the worst case (64x64 32bpp = 16kB) rather
	 * than the 2kB a 32x32 16bpp cursor needs, so the format stays tunable */
	if (si->settings.hardcursor) pointer_reservation = NV_CURSOR_MAX_BYTES;

	si->fbc.frame_buffer = (void *)((char *)si->framebuffer+pointer_reservation);
	si->fbc.frame_buffer_dma = (void *)((char *)si->framebuffer_pci+pointer_reservation);

	/* count of issued parameters or commands */
	si->engine.last_idle = si->engine.count = 0;
	/* no 3D clones are currently loaded */
	si->engine.threeD.clones = 0;
	/* tell 3D add-ons that they should reload their rendering states and surfaces */
	si->engine.threeD.reload = 0xffffffff;
	INIT_BEN(si->engine.lock);

	INIT_BEN(si->overlay.lock);
	for (cnt = 0; cnt < MAXBUFFERS; cnt++)
	{
		/* make sure overlay buffers are 'marked' as being free */
		si->overlay.myBuffer[cnt].buffer = NULL;
		si->overlay.myBuffer[cnt].buffer_dma = NULL;
	}

	/* make sure overlay unit is 'marked' as being free */
	si->overlay.myToken = NULL;	

	/* note that overlay is not in use (for nv_bes_move_overlay()) */
	si->overlay.active = false;

	/* initialise various cursor stuff */
	head1_cursor_init();
	if (si->ps.secondary_head) head2_cursor_init();

	/* ensure cursor state */
	head1_cursor_hide();
	if (si->ps.secondary_head) head2_cursor_hide();

	/* ensure DPMS state */
	si->dpms_flags = B_DPMS_ON;

	/* ensure TVout state:
	 * TVencoder is on head to be assigned primary, no dualhead switch mode active. */
	//fixme: actually check on what CRTC TVout was active during boot (if any)...
	si->dm.flags = TV_PRIMARY;

	/* make sure a possible 3D add-on will block rendering and re-initialize itself.
	 * note: update in _this_ order only */
	/* SET_DISPLAY_MODE will reset this flag when it's done. */
	si->engine.threeD.mode_changing = true;
	/* every 3D add-on will reset this bit-flag when it's done. */
	si->engine.threeD.newmode = 0xffffffff;

	/* we did not detect the Haiku ScreenPrefs app yet */
	si->haiku_prefs_used = false;
	si->Haiku_switch_head = false;

	/* a winner! */
	result = B_OK;
	/* ensure that INIT_ACCELERANT won't be executed again (copies should be clones) */
	si->accelerant_in_use = true;
	goto error0;

error1:
	/*
	Initialization failed after init_common() succeeded, so we need to clean
	up before quiting.
	*/
	uninit_common();

error0:
	return result;
}

/*
Return the number of bytes required to hold the information required
to clone the device.
*/
ssize_t ACCELERANT_CLONE_INFO_SIZE(void) {
	/*
	Since we're passing the name of the device as the only required
	info, return the size of the name buffer
	*/
	return B_OS_NAME_LENGTH; // apsed, was MAX_NV_DEVICE_NAME_LENGTH;
}


/*
Return the info required to clone the device.  void *data points to
a buffer at least ACCELERANT_CLONE_INFO_SIZE() bytes in length.
*/
void GET_ACCELERANT_CLONE_INFO(void *data) {
	nv_device_name dn;
	status_t result;

	/* call the kernel driver to get the device name */	
	dn.magic = NV_PRIVATE_DATA_MAGIC;
	/* store the returned info directly into the passed buffer */
	dn.name = (char *)data;
	result = ioctl(fd, NV_DEVICE_NAME, &dn, sizeof(dn));
}

/*
Initialize a copy of the accelerant as a clone.  void *data points to
a copy of the data returned by GET_ACCELERANT_CLONE_INFO().
*/
status_t CLONE_ACCELERANT(void *data)
{
	status_t result;
	char path[MAXPATHLEN];

	/* the data is the device name */
	/* Note: the R4 graphics driver kit is in error here (missing trailing '/') */
	strcpy(path, "/dev/");
	strcat(path, (const char *)data);
	/* open the device, the permissions aren't important */
	fd = open(path, B_READ_WRITE);
	if (fd < 0)
	{
		/* we can't use LOG because we didn't get the shared_info struct.. */
		char     fname[64];
		FILE    *myhand = NULL;

		sprintf (fname, "/boot/home/" DRIVER_PREFIX ".accelerant.0.log");
		myhand=fopen(fname,"a+");
		fprintf(myhand, "CLONE_ACCELERANT: couldn't open kerneldriver %s! Aborting.\n", path);
		fclose(myhand);

		/* abort with resultcode from open attempt on kerneldriver */
		result = fd;
		goto error0;
	}

	/* note that we're a clone accelerant */
	accelerantIsClone = 1;

	/* call the shared initialization code */
	result = init_common(fd);

	/* bail out if the common initialization failed */
	if (result != B_OK) goto error1;

	/* ensure that INIT_ACCELERANT is executed first (i.e. primary accelerant exists) */
	if (!(si->accelerant_in_use))
	{
		result = B_NOT_ALLOWED;
		goto error2;
	}

	/* setup CRTC and DAC functions access */
	//fixme: setup_virtualized_heads is a problem for clones: needs to be run
	//for each clone if the mode is changed!
	if (si->ps.secondary_head)
		setup_virtualized_heads(si->crtc_switch_mode);
	else
		setup_virtualized_heads(si->ps.crtc2_prim);

	/* get shared area for display modes */
	result = my_mode_list_area = clone_area(
		DRIVER_PREFIX " cloned display_modes",
		(void **)&my_mode_list,
		B_ANY_ADDRESS,
		B_READ_AREA,
		si->mode_area
	);
	if (result < B_OK) goto error2;

	/* all done */
	LOG(4,("CLONE_ACCELERANT: cloning was succesfull.\n"));

	result = B_OK;
	goto error0;

error2:
	/* free up the areas we cloned */
	uninit_common();
error1:
	/* close the device we opened */
	close(fd);
error0:
	return result;
}

void UNINIT_ACCELERANT(void)
{
	if (accelerantIsClone)
	{
		LOG(4,("UNINIT_ACCELERANT: shutting down clone accelerant.\n"));
	}
	else
	{
		LOG(4,("UNINIT_ACCELERANT: shutting down primary accelerant.\n"));

		/* delete benaphores ONLY if we are the primary accelerant */
		DELETE_BEN(si->engine.lock);
		DELETE_BEN(si->overlay.lock);

		/* ensure that INIT_ACCELERANT can be executed again */
		si->accelerant_in_use = false;
	}

	/* free our mode list area */
	delete_area(my_mode_list_area);
	/* paranoia */
	my_mode_list = 0;
	/* release our cloned data */
	uninit_common();
	/* close the file handle ONLY if we're the clone */
	if (accelerantIsClone) close(fd);
}
