/**
 * \file pcm/pcm_dsnoop.c
 * \ingroup PCM_Plugins
 * \brief PCM Capture Stream Snooping (dsnoop) Plugin Interface
 * \author Jaroslav Kysela <perex@perex.cz>
 * \date 2003
 */
/*
 *  PCM - Capture Stream Snooping
 *  Copyright (c) 2003 by Jaroslav Kysela <perex@perex.cz>
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as
 *   published by the Free Software Foundation; either version 2.1 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */
  
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <grp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include "pcm_direct.h"

#ifndef PIC
/* entry for static linking */
const char *_snd_module_pcm_dsnoop = "";
#endif

/*
 *
 */

static int snoop_timestamp(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	snd_pcm_uframes_t ptr1 = -2LL /* invalid value */, ptr2;

	/* loop is required to sync hw.ptr with timestamp */
	while (1) {
		ptr2 = *dsnoop->spcm->hw.ptr;
		if (ptr1 == ptr2)
			break;
		ptr1 = ptr2;
		dsnoop->update_tstamp = snd_pcm_hw_fast_tstamp(dsnoop->spcm);
	}
	dsnoop->slave_hw_ptr = ptr1;
	return 0;
}

static void snoop_areas(snd_pcm_direct_t *dsnoop,
			const snd_pcm_channel_area_t *src_areas,
			const snd_pcm_channel_area_t *dst_areas,
			snd_pcm_uframes_t src_ofs,
			snd_pcm_uframes_t dst_ofs,
			snd_pcm_uframes_t size)
{
	unsigned int chn, schn, channels;
	snd_pcm_format_t format;

	channels = dsnoop->channels;
	format = dsnoop->shmptr->s.format;
	if (dsnoop->interleaved) {
		unsigned int fbytes = snd_pcm_format_physical_width(format) / 8;
		memcpy(((char *)dst_areas[0].addr) + (dst_ofs * channels * fbytes),
		       ((char *)src_areas[0].addr) + (src_ofs * channels * fbytes),
		       size * channels * fbytes);
	} else {
		for (chn = 0; chn < channels; chn++) {
			schn = dsnoop->bindings ? dsnoop->bindings[chn] : chn;
			snd_pcm_area_copy(&dst_areas[chn], dst_ofs, &src_areas[schn], src_ofs, size, format);
		}
	}
}

/*
 *  synchronize shm ring buffer with hardware
 */
static void snd_pcm_dsnoop_sync_area(snd_pcm_t *pcm, snd_pcm_uframes_t slave_hw_ptr, snd_pcm_uframes_t size)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	snd_pcm_uframes_t hw_ptr = dsnoop->hw_ptr;
	snd_pcm_uframes_t transfer;
	const snd_pcm_channel_area_t *src_areas, *dst_areas;
	
	/* add sample areas here */
	dst_areas = snd_pcm_mmap_areas(pcm);
	src_areas = snd_pcm_mmap_areas(dsnoop->spcm);
	hw_ptr %= pcm->buffer_size;
	slave_hw_ptr %= dsnoop->slave_buffer_size;
	while (size > 0) {
		transfer = hw_ptr + size > pcm->buffer_size ? pcm->buffer_size - hw_ptr : size;
		transfer = slave_hw_ptr + transfer > dsnoop->slave_buffer_size ?
			dsnoop->slave_buffer_size - slave_hw_ptr : transfer;
		size -= transfer;
		snoop_areas(dsnoop, src_areas, dst_areas, slave_hw_ptr, hw_ptr, transfer);
		slave_hw_ptr += transfer;
	 	slave_hw_ptr %= dsnoop->slave_buffer_size;
		hw_ptr += transfer;
		hw_ptr %= pcm->buffer_size;
	}
}

/*
 *  synchronize hardware pointer (hw_ptr) with ours
 */
static int snd_pcm_dsnoop_sync_ptr(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	snd_pcm_uframes_t slave_hw_ptr, old_slave_hw_ptr, avail;
	snd_pcm_sframes_t diff;
	int err;

	if (dsnoop->slowptr)
		snd_pcm_hwsync(dsnoop->spcm);
	old_slave_hw_ptr = dsnoop->slave_hw_ptr;
	snoop_timestamp(pcm);
	slave_hw_ptr = dsnoop->slave_hw_ptr;
	err = snd_pcm_direct_check_xrun(dsnoop, pcm);
	if (err < 0)
		return err;
	diff = pcm_frame_diff(slave_hw_ptr, old_slave_hw_ptr, dsnoop->slave_boundary);
	if (diff == 0)		/* fast path */
		return 0;
	snd_pcm_dsnoop_sync_area(pcm, old_slave_hw_ptr, diff);
	dsnoop->hw_ptr += diff;
	dsnoop->hw_ptr %= pcm->boundary;
	// printf("sync ptr diff = %li\n", diff);
	if (pcm->stop_threshold >= pcm->boundary)	/* don't care */
		return 0;
	if ((avail = snd_pcm_mmap_capture_avail(pcm)) >= pcm->stop_threshold) {
		gettimestamp(&dsnoop->trigger_tstamp, pcm->tstamp_type);
		dsnoop->state = SND_PCM_STATE_XRUN;
		dsnoop->avail_max = avail;
		return -EPIPE;
	}
	if (avail > dsnoop->avail_max)
		dsnoop->avail_max = avail;
	return 0;
}

/*
 *  plugin implementation
 */

static int snd_pcm_dsnoop_status(snd_pcm_t *pcm, snd_pcm_status_t * status)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	snd_pcm_state_t state;

	switch(dsnoop->state) {
	case SNDRV_PCM_STATE_DRAINING:
	case SNDRV_PCM_STATE_RUNNING:
		snd_pcm_dsnoop_sync_ptr(pcm);
		break;
	default:
		break;
	}
	memset(status, 0, sizeof(*status));
	snd_pcm_status(dsnoop->spcm, status);
	state = snd_pcm_state(dsnoop->spcm);
	status->state = state == SND_PCM_STATE_RUNNING ? dsnoop->state : state;
	status->hw_ptr = *pcm->hw.ptr; /* boundary may be different */
	status->appl_ptr = *pcm->appl.ptr; /* slave PCM doesn't set appl_ptr */
	status->trigger_tstamp = dsnoop->trigger_tstamp;
	status->avail = snd_pcm_mmap_capture_avail(pcm);
	status->avail_max = status->avail > dsnoop->avail_max ? status->avail : dsnoop->avail_max;
	dsnoop->avail_max = 0;
	status->delay = snd_pcm_mmap_capture_delay(pcm);
	return 0;
}

static snd_pcm_state_t snd_pcm_dsnoop_state(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;

	snd_pcm_direct_check_xrun(dsnoop, pcm);
	return dsnoop->state;
}

static int snd_pcm_dsnoop_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delayp)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	int err;
	
	switch(dsnoop->state) {
	case SNDRV_PCM_STATE_DRAINING:
	case SNDRV_PCM_STATE_RUNNING:
		err = snd_pcm_dsnoop_sync_ptr(pcm);
		if (err < 0)
			return err;
		/* Fall through */
	case SNDRV_PCM_STATE_PREPARED:
	case SNDRV_PCM_STATE_SUSPENDED:
		*delayp = snd_pcm_mmap_capture_avail(pcm);
		return 0;
	case SNDRV_PCM_STATE_XRUN:
		return -EPIPE;
	case SNDRV_PCM_STATE_DISCONNECTED:
		return -ENODEV;
	default:
		return -EBADFD;
	}
}

static int snd_pcm_dsnoop_hwsync(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;

	switch(dsnoop->state) {
	case SNDRV_PCM_STATE_DRAINING:
	case SNDRV_PCM_STATE_RUNNING:
		return snd_pcm_dsnoop_sync_ptr(pcm);
	case SNDRV_PCM_STATE_PREPARED:
	case SNDRV_PCM_STATE_SUSPENDED:
		return 0;
	case SNDRV_PCM_STATE_XRUN:
		return -EPIPE;
	case SNDRV_PCM_STATE_DISCONNECTED:
		return -ENODEV;
	default:
		return -EBADFD;
	}
}

static int snd_pcm_dsnoop_reset(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	dsnoop->hw_ptr %= pcm->period_size;
	dsnoop->appl_ptr = dsnoop->hw_ptr;
	dsnoop->slave_appl_ptr = dsnoop->slave_hw_ptr;
	snd_pcm_direct_reset_slave_ptr(pcm, dsnoop);
	return 0;
}

static int snd_pcm_dsnoop_start(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	int err;

	if (dsnoop->state != SND_PCM_STATE_PREPARED)
		return -EBADFD;
	snd_pcm_hwsync(dsnoop->spcm);
	snoop_timestamp(pcm);
	dsnoop->slave_appl_ptr = dsnoop->slave_hw_ptr;
	snd_pcm_direct_reset_slave_ptr(pcm, dsnoop);
	err = snd_timer_start(dsnoop->timer);
	if (err < 0)
		return err;
	dsnoop->state = SND_PCM_STATE_RUNNING;
	dsnoop->trigger_tstamp = dsnoop->update_tstamp;
	return 0;
}

static int snd_pcm_dsnoop_drop(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	if (dsnoop->state == SND_PCM_STATE_OPEN)
		return -EBADFD;
	dsnoop->state = SND_PCM_STATE_SETUP;
	snd_timer_stop(dsnoop->timer);
	return 0;
}

/* locked version */
static int __snd_pcm_dsnoop_drain(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	snd_pcm_uframes_t stop_threshold;
	int err;

	if (dsnoop->state == SND_PCM_STATE_OPEN)
		return -EBADFD;
	stop_threshold = pcm->stop_threshold;
	if (pcm->stop_threshold > pcm->buffer_size)
		pcm->stop_threshold = pcm->buffer_size;
	while (dsnoop->state == SND_PCM_STATE_RUNNING) {
		err = snd_pcm_dsnoop_sync_ptr(pcm);
		if (err < 0)
			break;
		if (pcm->mode & SND_PCM_NONBLOCK)
			return -EAGAIN;
		__snd_pcm_wait_in_lock(pcm, -1);
	}
	pcm->stop_threshold = stop_threshold;
	return snd_pcm_dsnoop_drop(pcm);
}

static int snd_pcm_dsnoop_drain(snd_pcm_t *pcm)
{
	int err;

	snd_pcm_lock(pcm);
	err = __snd_pcm_dsnoop_drain(pcm);
	snd_pcm_unlock(pcm);
	return err;
}

static int snd_pcm_dsnoop_pause(snd_pcm_t *pcm ATTRIBUTE_UNUSED, int enable ATTRIBUTE_UNUSED)
{
	return -EIO;
}

static snd_pcm_sframes_t snd_pcm_dsnoop_rewindable(snd_pcm_t *pcm)
{
	return snd_pcm_mmap_capture_hw_rewindable(pcm);
}

static snd_pcm_sframes_t snd_pcm_dsnoop_rewind(snd_pcm_t *pcm, snd_pcm_uframes_t frames)
{
	snd_pcm_sframes_t avail;

	avail = snd_pcm_dsnoop_rewindable(pcm);
	if (frames > (snd_pcm_uframes_t)avail)
		frames = avail;
	snd_pcm_mmap_appl_backward(pcm, frames);
	return frames;
}

static snd_pcm_sframes_t snd_pcm_dsnoop_forwardable(snd_pcm_t *pcm)
{
	return snd_pcm_mmap_capture_avail(pcm);
}

static snd_pcm_sframes_t snd_pcm_dsnoop_forward(snd_pcm_t *pcm, snd_pcm_uframes_t frames)
{
	snd_pcm_sframes_t avail;

	avail = snd_pcm_dsnoop_forwardable(pcm);
	if (frames > (snd_pcm_uframes_t)avail)
		frames = avail;
	snd_pcm_mmap_appl_forward(pcm, frames);
	return frames;
}

static snd_pcm_sframes_t snd_pcm_dsnoop_writei(snd_pcm_t *pcm ATTRIBUTE_UNUSED, const void *buffer ATTRIBUTE_UNUSED, snd_pcm_uframes_t size ATTRIBUTE_UNUSED)
{
	return -ENODEV;
}

static snd_pcm_sframes_t snd_pcm_dsnoop_writen(snd_pcm_t *pcm ATTRIBUTE_UNUSED, void **bufs ATTRIBUTE_UNUSED, snd_pcm_uframes_t size ATTRIBUTE_UNUSED)
{
	return -ENODEV;
}

static int snd_pcm_dsnoop_close(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;

	if (dsnoop->timer)
		snd_timer_close(dsnoop->timer);
	snd_pcm_direct_semaphore_down(dsnoop, DIRECT_IPC_SEM_CLIENT);
	snd_pcm_close(dsnoop->spcm);
 	if (dsnoop->server)
 		snd_pcm_direct_server_discard(dsnoop);
 	if (dsnoop->client)
 		snd_pcm_direct_client_discard(dsnoop);
	if (snd_pcm_direct_shm_discard(dsnoop)) {
		if (snd_pcm_direct_semaphore_discard(dsnoop))
			snd_pcm_direct_semaphore_final(dsnoop, DIRECT_IPC_SEM_CLIENT);
	} else
		snd_pcm_direct_semaphore_final(dsnoop, DIRECT_IPC_SEM_CLIENT);
	free(dsnoop->bindings);
	pcm->private_data = NULL;
	free(dsnoop);
	return 0;
}

static snd_pcm_sframes_t snd_pcm_dsnoop_mmap_commit(snd_pcm_t *pcm,
						    snd_pcm_uframes_t offset ATTRIBUTE_UNUSED,
						    snd_pcm_uframes_t size)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	int err;

	err = snd_pcm_direct_check_xrun(dsnoop, pcm);
	if (err < 0)
		return err;
	if (dsnoop->state == SND_PCM_STATE_RUNNING) {
		err = snd_pcm_dsnoop_sync_ptr(pcm);
		if (err < 0)
			return err;
	}
	snd_pcm_mmap_appl_forward(pcm, size);
	/* clear timer queue to avoid a bogus return from poll */
	if (snd_pcm_mmap_capture_avail(pcm) < pcm->avail_min)
		snd_pcm_direct_clear_timer_queue(dsnoop);
	return size;
}

static snd_pcm_sframes_t snd_pcm_dsnoop_avail_update(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	int err;
	
	if (dsnoop->state == SND_PCM_STATE_RUNNING) {
		err = snd_pcm_dsnoop_sync_ptr(pcm);
		if (err < 0)
			return err;
	}
	if (dsnoop->state == SND_PCM_STATE_XRUN)
		return -EPIPE;

	return snd_pcm_mmap_capture_avail(pcm);
}

static int snd_pcm_dsnoop_htimestamp(snd_pcm_t *pcm,
				     snd_pcm_uframes_t *avail,
				     snd_htimestamp_t *tstamp)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	snd_pcm_uframes_t avail1;
	int ok = 0;
	
	while (1) {
		if (dsnoop->state == SND_PCM_STATE_RUNNING ||
		    dsnoop->state == SND_PCM_STATE_DRAINING)
			snd_pcm_dsnoop_sync_ptr(pcm);
		avail1 = snd_pcm_mmap_capture_avail(pcm);
		if (ok && *avail == avail1)
			break;
		*avail = avail1;
		*tstamp = snd_pcm_hw_fast_tstamp(dsnoop->spcm);
		ok = 1;
	}
	return 0;
}

static void snd_pcm_dsnoop_dump(snd_pcm_t *pcm, snd_output_t *out)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;

	snd_output_printf(out, "Direct Snoop PCM\n");
	if (pcm->setup) {
		snd_output_printf(out, "Its setup is:\n");
		snd_pcm_dump_setup(pcm, out);
	}
	if (dsnoop->spcm)
		snd_pcm_dump(dsnoop->spcm, out);
}

static const snd_pcm_ops_t snd_pcm_dsnoop_ops = {
	.close = snd_pcm_dsnoop_close,
	.info = snd_pcm_direct_info,
	.hw_refine = snd_pcm_direct_hw_refine,
	.hw_params = snd_pcm_direct_hw_params,
	.hw_free = snd_pcm_direct_hw_free,
	.sw_params = snd_pcm_direct_sw_params,
	.channel_info = snd_pcm_direct_channel_info,
	.dump = snd_pcm_dsnoop_dump,
	.nonblock = snd_pcm_direct_nonblock,
	.async = snd_pcm_direct_async,
	.mmap = snd_pcm_direct_mmap,
	.munmap = snd_pcm_direct_munmap,
	.query_chmaps = snd_pcm_direct_query_chmaps,
	.get_chmap = snd_pcm_direct_get_chmap,
	.set_chmap = snd_pcm_direct_set_chmap,
};

static const snd_pcm_fast_ops_t snd_pcm_dsnoop_fast_ops = {
	.status = snd_pcm_dsnoop_status,
	.state = snd_pcm_dsnoop_state,
	.hwsync = snd_pcm_dsnoop_hwsync,
	.delay = snd_pcm_dsnoop_delay,
	.prepare = snd_pcm_direct_prepare,
	.reset = snd_pcm_dsnoop_reset,
	.start = snd_pcm_dsnoop_start,
	.drop = snd_pcm_dsnoop_drop,
	.drain = snd_pcm_dsnoop_drain,
	.pause = snd_pcm_dsnoop_pause,
	.rewindable = snd_pcm_dsnoop_rewindable,
	.rewind = snd_pcm_dsnoop_rewind,
	.forwardable = snd_pcm_dsnoop_forwardable,
	.forward = snd_pcm_dsnoop_forward,
	.resume = snd_pcm_direct_resume,
	.link = NULL,
	.link_slaves = NULL,
	.unlink = NULL,
	.writei = snd_pcm_dsnoop_writei,
	.writen = snd_pcm_dsnoop_writen,
	.readi = snd_pcm_mmap_readi,
	.readn = snd_pcm_mmap_readn,
	.avail_update = snd_pcm_dsnoop_avail_update,
	.mmap_commit = snd_pcm_dsnoop_mmap_commit,
	.htimestamp = snd_pcm_dsnoop_htimestamp,
	.poll_descriptors = snd_pcm_direct_poll_descriptors,
	.poll_descriptors_count = NULL,
	.poll_revents = snd_pcm_direct_poll_revents,
};

/**
 * \brief Creates a new dsnoop PCM
 * \param pcmp Returns created PCM handle
 * \param name Name of PCM
 * \param opts Direct PCM configurations
 * \param params Parameters for slave
 * \param root Configuration root
 * \param sconf Slave configuration
 * \param stream PCM Direction (stream)
 * \param mode PCM Mode
 * \retval zero on success otherwise a negative error code
 * \warning Using of this function might be dangerous in the sense
 *          of compatibility reasons. The prototype might be freely
 *          changed in future.
 */
int snd_pcm_dsnoop_open(snd_pcm_t **pcmp, const char *name,
			struct snd_pcm_direct_open_conf *opts,
			struct slave_params *params,
			snd_config_t *root, snd_config_t *sconf,
			snd_pcm_stream_t stream, int mode)
{
	snd_pcm_t *pcm, *spcm = NULL;
	snd_pcm_direct_t *dsnoop;
	int ret, first_instance;

	assert(pcmp);

	if (stream != SND_PCM_STREAM_CAPTURE) {
		SNDERR("The dsnoop plugin supports only capture stream");
		return -EINVAL;
	}

	ret = _snd_pcm_direct_new(&pcm, &dsnoop, SND_PCM_TYPE_DSNOOP, name, opts, params, stream, mode);
	if (ret < 0)
		return ret;
	first_instance = ret;

	pcm->ops = &snd_pcm_dsnoop_ops;
	pcm->fast_ops = &snd_pcm_dsnoop_fast_ops;
	pcm->private_data = dsnoop;
	dsnoop->state = SND_PCM_STATE_OPEN;
	dsnoop->slowptr = opts->slowptr;
	dsnoop->max_periods = opts->max_periods;
	dsnoop->var_periodsize = opts->var_periodsize;
	dsnoop->sync_ptr = snd_pcm_dsnoop_sync_ptr;
	dsnoop->hw_ptr_alignment = opts->hw_ptr_alignment;

 retry:
	if (first_instance) {
		/* recursion is already checked in
		   snd_pcm_direct_get_slave_ipc_offset() */
		ret = snd_pcm_open_slave(&spcm, root, sconf, stream,
					 mode | SND_PCM_NONBLOCK, NULL);
		if (ret < 0) {
			SNDERR("unable to open slave");
			goto _err;
		}
	
		if (snd_pcm_type(spcm) != SND_PCM_TYPE_HW) {
			SNDERR("dsnoop plugin can be only connected to hw plugin");
			goto _err;
		}
		
		ret = snd_pcm_direct_initialize_slave(dsnoop, spcm, params);
		if (ret < 0) {
			SNDERR("unable to initialize slave");
			goto _err;
		}

		dsnoop->spcm = spcm;
		
		if (dsnoop->shmptr->use_server) {
			ret = snd_pcm_direct_server_create(dsnoop);
			if (ret < 0) {
				SNDERR("unable to create server");
				goto _err;
			}
		}

		dsnoop->shmptr->type = spcm->type;
	} else {
		if (dsnoop->shmptr->use_server) {
			/* up semaphore to avoid deadlock */
			snd_pcm_direct_semaphore_up(dsnoop, DIRECT_IPC_SEM_CLIENT);
			ret = snd_pcm_direct_client_connect(dsnoop);
			if (ret < 0) {
				SNDERR("unable to connect client");
				goto _err_nosem;
			}
			
			snd_pcm_direct_semaphore_down(dsnoop, DIRECT_IPC_SEM_CLIENT);

			ret = snd_pcm_direct_open_secondary_client(&spcm, dsnoop, "dsnoop_client");
			if (ret < 0)
				goto _err;
		} else {

			ret = snd_pcm_open_slave(&spcm, root, sconf, stream,
						 mode | SND_PCM_NONBLOCK |
						 SND_PCM_APPEND,
						 NULL);
			if (ret < 0) {
				/* all other streams have been closed;
				 * retry as the first instance
				 */
				if (ret == -EBADFD) {
					first_instance = 1;
					goto retry;
				}
				SNDERR("unable to open slave");
				goto _err;
			}
			if (snd_pcm_type(spcm) != SND_PCM_TYPE_HW) {
				SNDERR("dsnoop plugin can be only connected to hw plugin");
				ret = -EINVAL;
				goto _err;
			}
		
			ret = snd_pcm_direct_initialize_secondary_slave(dsnoop, spcm, params);
			if (ret < 0) {
				SNDERR("unable to initialize slave");
				goto _err;
			}
		}

		dsnoop->spcm = spcm;
	}

	ret = snd_pcm_direct_initialize_poll_fd(dsnoop);
	if (ret < 0) {
		SNDERR("unable to initialize poll_fd");
		goto _err;
	}

	pcm->poll_fd = dsnoop->poll_fd;
	pcm->poll_events = POLLIN;	/* it's different than other plugins */
	pcm->tstamp_type = spcm->tstamp_type;
	pcm->mmap_rw = 1;
	snd_pcm_set_hw_ptr(pcm, &dsnoop->hw_ptr, -1, 0);
	snd_pcm_set_appl_ptr(pcm, &dsnoop->appl_ptr, -1, 0);
	
	if (dsnoop->channels == UINT_MAX)
		dsnoop->channels = dsnoop->shmptr->s.channels;
	
	snd_pcm_direct_semaphore_up(dsnoop, DIRECT_IPC_SEM_CLIENT);

	*pcmp = pcm;
	return 0;
	
 _err:
 	if (dsnoop->timer)
		snd_timer_close(dsnoop->timer);
	if (dsnoop->server)
		snd_pcm_direct_server_discard(dsnoop);
	if (dsnoop->client)
		snd_pcm_direct_client_discard(dsnoop);
	if (spcm)
		snd_pcm_close(spcm);
	if ((dsnoop->shmid >= 0) && (snd_pcm_direct_shm_discard(dsnoop))) {
		if (snd_pcm_direct_semaphore_discard(dsnoop))
			snd_pcm_direct_semaphore_final(dsnoop, DIRECT_IPC_SEM_CLIENT);
	} else
		snd_pcm_direct_semaphore_up(dsnoop, DIRECT_IPC_SEM_CLIENT);

 _err_nosem:
	free(dsnoop->bindings);
	free(dsnoop);
	snd_pcm_free(pcm);
	return ret;
}

/*! \page pcm_plugins

\section pcm_plugins_dsnoop Plugin: dsnoop

This plugin splits one capture stream to more.
It works the reverse way of \ref pcm_plugins_dmix "dmix plugin",
reading the shared capture buffer from many clients concurrently.
The meaning of parameters below are almost identical with
dmix plugin.

\code
pcm.name {
	type dsnoop		# Direct snoop
	ipc_key INT		# unique IPC key
	ipc_key_add_uid BOOL	# add current uid to unique IPC key
	ipc_perm INT		# IPC permissions (octal, default 0600)
	hw_ptr_alignment STR	# Slave application and hw pointer alignment type
		# STR can be one of the below strings :
		# no
		# roundup
		# rounddown
		# auto (default)
	tstamp_type STR		# timestamp type
				# STR can be one of the below strings :
				# default, gettimeofday, monotonic, monotonic_raw
	slave STR
	# or
	slave {			# Slave definition
		pcm STR		# slave PCM name
		# or
		pcm { }		# slave PCM definition
		format STR	# format definition
		rate INT	# rate definition
		channels INT
		period_time INT	# in usec
		# or
		period_size INT	# in frames
		buffer_time INT	# in usec
		# or
		buffer_size INT # in frames
		periods INT	# when buffer_size or buffer_time is not specified
	}
	bindings {		# note: this is client independent!!!
		N INT		# maps slave channel to client channel N
	}
	slowptr BOOL		# slow but more precise pointer updates
}
\endcode

<code>hw_ptr_alignment</code> specifies slave application and hw
pointer alignment type. By default hw_ptr_alignment is auto. Below are
the possible configurations:
- no: minimal latency with minimal frames dropped at startup. But
  wakeup of application (return from snd_pcm_wait() or poll()) can
  take up to 2 * period.
- roundup: It is guaranteed that all frames will be played at
  startup. But the latency will increase upto period-1 frames.
- rounddown: It is guaranteed that a wakeup will happen for each
  period and frames can be written from application. But on startup
  upto period-1 frames will be dropped.
- auto: Selects the best approach depending on the used period and
  buffer size.
  If the application buffer size is < 2 * application period,
  "roundup" will be selected to avoid over runs. If the slave_period
  is < 10ms we could expect that there are low latency
  requirements. Therefore "rounddown" will be chosen to avoid long
  wakeup times. Else "no" will be chosen.

\subsection pcm_plugins_dsnoop_funcref Function reference

<UL>
  <LI>snd_pcm_dsnoop_open()
  <LI>_snd_pcm_dsnoop_open()
</UL>

*/

/**
 * \brief Creates a new dsnoop PCM
 * \param pcmp Returns created PCM handle
 * \param name Name of PCM
 * \param root Root configuration node
 * \param conf Configuration node with dsnoop PCM description
 * \param stream PCM Stream
 * \param mode PCM Mode
 * \warning Using of this function might be dangerous in the sense
 *          of compatibility reasons. The prototype might be freely
 *          changed in future.
 */
int _snd_pcm_dsnoop_open(snd_pcm_t **pcmp, const char *name,
		       snd_config_t *root, snd_config_t *conf,
		       snd_pcm_stream_t stream, int mode)
{
	snd_config_t *sconf;
	struct slave_params params;
	struct snd_pcm_direct_open_conf dopen;
	int bsize, psize;
	int err;

	err = snd_pcm_direct_parse_open_conf(root, conf, stream, &dopen);
	if (err < 0)
		return err;

	/* the default settings, it might be invalid for some hardware */
	params.format = SND_PCM_FORMAT_S16;
	params.rate = 48000;
	params.channels = 2;
	params.period_time = -1;
	params.buffer_time = -1;
	bsize = psize = -1;
	params.periods = 3;
	err = snd_pcm_slave_conf(root, dopen.slave, &sconf, 8,
				 SND_PCM_HW_PARAM_FORMAT, SCONF_UNCHANGED, &params.format,
				 SND_PCM_HW_PARAM_RATE, 0, &params.rate,
				 SND_PCM_HW_PARAM_CHANNELS, 0, &params.channels,
				 SND_PCM_HW_PARAM_PERIOD_TIME, 0, &params.period_time,
				 SND_PCM_HW_PARAM_BUFFER_TIME, 0, &params.buffer_time,
				 SND_PCM_HW_PARAM_PERIOD_SIZE, 0, &psize,
				 SND_PCM_HW_PARAM_BUFFER_SIZE, 0, &bsize,
				 SND_PCM_HW_PARAM_PERIODS, 0, &params.periods);
	if (err < 0)
		return err;

	/* set a reasonable default */  
	if (psize == -1 && params.period_time == -1)
		params.period_time = 125000;    /* 0.125 seconds */

	if (params.format == -2)
		params.format = SND_PCM_FORMAT_UNKNOWN;

	params.period_size = psize;
	params.buffer_size = bsize;

	err = snd_pcm_dsnoop_open(pcmp, name, &dopen, &params,
				  root, sconf, stream, mode);
	snd_config_delete(sconf);
	return err;
}
#ifndef DOC_HIDDEN
SND_DLSYM_BUILD_VERSION(_snd_pcm_dsnoop_open, SND_PCM_DLSYM_VERSION);
#endif
