/**
 * \file pcm/pcm_dsnoop.c
 * \ingroup PCM_Plugins
 * \brief PCM Capture Stream Snooping (dsnoop) Plugin Interface
 * \author Jaroslav Kysela <perex@suse.cz>
 * \date 2003
 */
/*
 *  PCM - Capture Stream Snooping
 *  Copyright (c) 2003 by Jaroslav Kysela <perex@suse.cz>
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
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
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
	slave_hw_ptr %= dsnoop->shmptr->s.buffer_size;
	while (size > 0) {
		transfer = hw_ptr + size > pcm->buffer_size ? pcm->buffer_size - hw_ptr : size;
		transfer = slave_hw_ptr + transfer > dsnoop->shmptr->s.buffer_size ? dsnoop->shmptr->s.buffer_size - slave_hw_ptr : transfer;
		size -= transfer;
		snoop_areas(dsnoop, src_areas, dst_areas, slave_hw_ptr, hw_ptr, transfer);
		slave_hw_ptr += transfer;
	 	slave_hw_ptr %= dsnoop->shmptr->s.buffer_size;
		hw_ptr += transfer;
		hw_ptr %= pcm->buffer_size;
	}
}

/*
 *  synchronize hardware pointer (hw_ptr) with ours
 */
static snd_pcm_sframes_t snd_pcm_dsnoop_sync_ptr(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	snd_pcm_uframes_t slave_hw_ptr, old_slave_hw_ptr, avail;
	snd_pcm_sframes_t diff;
	
	switch (snd_pcm_state(dsnoop->spcm)) {
	case SND_PCM_STATE_SUSPENDED:
		return -ESTRPIPE;
	case SND_PCM_STATE_DISCONNECTED:
		dsnoop->state = SNDRV_PCM_STATE_DISCONNECTED;
		return -ENOTTY;
	default:
		break;
	}
	if (dsnoop->slowptr)
		snd_pcm_hwsync(dsnoop->spcm);
	old_slave_hw_ptr = dsnoop->slave_hw_ptr;
	slave_hw_ptr = dsnoop->slave_hw_ptr = *dsnoop->spcm->hw.ptr;
	diff = slave_hw_ptr - old_slave_hw_ptr;
	if (diff == 0)		/* fast path */
		return 0;
	if (diff < 0) {
		slave_hw_ptr += dsnoop->shmptr->s.boundary;
		diff = slave_hw_ptr - old_slave_hw_ptr;
	}
	snd_pcm_dsnoop_sync_area(pcm, old_slave_hw_ptr, diff);
	dsnoop->hw_ptr += diff;
	dsnoop->hw_ptr %= pcm->boundary;
	// printf("sync ptr diff = %li\n", diff);
	if (pcm->stop_threshold >= pcm->boundary)	/* don't care */
		return 0;
	if ((avail = snd_pcm_mmap_capture_hw_avail(pcm)) >= pcm->stop_threshold) {
		struct timeval tv;
		gettimeofday(&tv, 0);
		dsnoop->trigger_tstamp.tv_sec = tv.tv_sec;
		dsnoop->trigger_tstamp.tv_nsec = tv.tv_usec * 1000L;
		dsnoop->state = SND_PCM_STATE_XRUN;
		dsnoop->avail_max = avail;
		return -EPIPE;
	}
	if (avail > dsnoop->avail_max)
		dsnoop->avail_max = avail;
	return diff;
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
	state = snd_pcm_state(dsnoop->spcm);
	status->state = state == SND_PCM_STATE_RUNNING ? dsnoop->state : state;
	status->trigger_tstamp = dsnoop->trigger_tstamp;
	status->tstamp = snd_pcm_hw_fast_tstamp(dsnoop->spcm);
	status->avail = snd_pcm_mmap_capture_avail(pcm);
	status->avail_max = status->avail > dsnoop->avail_max ? status->avail : dsnoop->avail_max;
	dsnoop->avail_max = 0;
	return 0;
}

static snd_pcm_state_t snd_pcm_dsnoop_state(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	switch (snd_pcm_state(dsnoop->spcm)) {
	case SND_PCM_STATE_SUSPENDED:
		return -ESTRPIPE;
	case SND_PCM_STATE_DISCONNECTED:
		dsnoop->state = SNDRV_PCM_STATE_DISCONNECTED;
		return -ENOTTY;
	default:
		break;
	}
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
	case SNDRV_PCM_STATE_PREPARED:
	case SNDRV_PCM_STATE_SUSPENDED:
		*delayp = snd_pcm_mmap_capture_hw_avail(pcm);
		return 0;
	case SNDRV_PCM_STATE_XRUN:
		return -EPIPE;
	case SNDRV_PCM_STATE_DISCONNECTED:
		return -ENOTTY;
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
		return -ENOTTY;
	default:
		return -EBADFD;
	}
}

static int snd_pcm_dsnoop_prepare(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;

	snd_pcm_direct_check_interleave(dsnoop, pcm);
	// assert(pcm->boundary == dsnoop->shmptr->s.boundary);	/* for sure */
	dsnoop->state = SND_PCM_STATE_PREPARED;
	dsnoop->appl_ptr = 0;
	dsnoop->hw_ptr = 0;
	return 0;
}

static int snd_pcm_dsnoop_reset(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	dsnoop->hw_ptr %= pcm->period_size;
	dsnoop->appl_ptr = dsnoop->hw_ptr;
	dsnoop->slave_appl_ptr = dsnoop->slave_hw_ptr = *dsnoop->spcm->hw.ptr;
	return 0;
}

static int snd_pcm_dsnoop_start(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	struct timeval tv;
	int err;
	
	if (dsnoop->state != SND_PCM_STATE_PREPARED)
		return -EBADFD;
	err = snd_timer_start(dsnoop->timer);
	if (err < 0)
		return err;
	dsnoop->state = SND_PCM_STATE_RUNNING;
	dsnoop->slave_appl_ptr = dsnoop->slave_hw_ptr = *dsnoop->spcm->hw.ptr;
	gettimeofday(&tv, 0);
	dsnoop->trigger_tstamp.tv_sec = tv.tv_sec;
	dsnoop->trigger_tstamp.tv_nsec = tv.tv_usec * 1000L;
	return 0;
}

static int snd_pcm_dsnoop_drop(snd_pcm_t *pcm)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	if (dsnoop->state == SND_PCM_STATE_OPEN)
		return -EBADFD;
	snd_timer_stop(dsnoop->timer);
	dsnoop->state = SND_PCM_STATE_SETUP;
	return 0;
}

static int snd_pcm_dsnoop_drain(snd_pcm_t *pcm)
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
		snd_pcm_wait(pcm, -1);
	}
	pcm->stop_threshold = stop_threshold;
	return snd_pcm_dsnoop_drop(pcm);
}

static int snd_pcm_dsnoop_pause(snd_pcm_t *pcm, int enable)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
        if (enable) {
		if (dsnoop->state != SND_PCM_STATE_RUNNING)
			return -EBADFD;
		dsnoop->state = SND_PCM_STATE_PAUSED;
		snd_timer_stop(dsnoop->timer);
	} else {
		if (dsnoop->state != SND_PCM_STATE_PAUSED)
			return -EBADFD;
                dsnoop->state = SND_PCM_STATE_RUNNING;
                snd_timer_start(dsnoop->timer);
	}
	return 0;
}

static snd_pcm_sframes_t snd_pcm_dsnoop_rewind(snd_pcm_t *pcm, snd_pcm_uframes_t frames)
{
	snd_pcm_mmap_appl_backward(pcm, frames);
	return frames;
}

static snd_pcm_sframes_t snd_pcm_dsnoop_forward(snd_pcm_t *pcm, snd_pcm_uframes_t frames)
{
	snd_pcm_sframes_t avail;

	avail = snd_pcm_mmap_capture_hw_avail(pcm);
	if (avail < 0)
		return 0;
	if (frames > (snd_pcm_uframes_t)avail)
		frames = avail;
	snd_pcm_mmap_appl_forward(pcm, frames);
	return frames;
}

static int snd_pcm_dsnoop_resume(snd_pcm_t *pcm ATTRIBUTE_UNUSED)
{
	// snd_pcm_direct_t *dsnoop = pcm->private_data;
	// FIXME
	return 0;
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
 	if (snd_pcm_direct_shm_discard(dsnoop) > 0) {
 		if (snd_pcm_direct_semaphore_discard(dsnoop) < 0)
 			snd_pcm_direct_semaphore_up(dsnoop, DIRECT_IPC_SEM_CLIENT);
 	} else {
		snd_pcm_direct_semaphore_up(dsnoop, DIRECT_IPC_SEM_CLIENT);
	}
	if (dsnoop->bindings)
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

	if (dsnoop->state == SND_PCM_STATE_RUNNING) {
		err = snd_pcm_dsnoop_sync_ptr(pcm);
		if (err < 0)
			return err;
	}
	snd_pcm_mmap_appl_forward(pcm, size);
	return size;
}

static snd_pcm_sframes_t snd_pcm_dsnoop_avail_update(snd_pcm_t *pcm ATTRIBUTE_UNUSED)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;
	int err;
	
	if (dsnoop->state == SND_PCM_STATE_RUNNING) {
		err = snd_pcm_dsnoop_sync_ptr(pcm);
		if (err < 0)
			return err;
	}
	return snd_pcm_mmap_capture_avail(pcm);
}

static void snd_pcm_dsnoop_dump(snd_pcm_t *pcm, snd_output_t *out)
{
	snd_pcm_direct_t *dsnoop = pcm->private_data;

	snd_output_printf(out, "Direct Stream Mixing PCM\n");
	if (pcm->setup) {
		snd_output_printf(out, "\nIts setup is:\n");
		snd_pcm_dump_setup(pcm, out);
	}
	if (dsnoop->spcm)
		snd_pcm_dump(dsnoop->spcm, out);
}

static snd_pcm_ops_t snd_pcm_dsnoop_ops = {
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
	.poll_revents = snd_pcm_direct_poll_revents,
	.mmap = snd_pcm_direct_mmap,
	.munmap = snd_pcm_direct_munmap,
};

static snd_pcm_fast_ops_t snd_pcm_dsnoop_fast_ops = {
	.status = snd_pcm_dsnoop_status,
	.state = snd_pcm_dsnoop_state,
	.hwsync = snd_pcm_dsnoop_hwsync,
	.delay = snd_pcm_dsnoop_delay,
	.prepare = snd_pcm_dsnoop_prepare,
	.reset = snd_pcm_dsnoop_reset,
	.start = snd_pcm_dsnoop_start,
	.drop = snd_pcm_dsnoop_drop,
	.drain = snd_pcm_dsnoop_drain,
	.pause = snd_pcm_dsnoop_pause,
	.rewind = snd_pcm_dsnoop_rewind,
	.forward = snd_pcm_dsnoop_forward,
	.resume = snd_pcm_dsnoop_resume,
	.writei = snd_pcm_dsnoop_writei,
	.writen = snd_pcm_dsnoop_writen,
	.readi = snd_pcm_mmap_readi,
	.readn = snd_pcm_mmap_readn,
	.avail_update = snd_pcm_dsnoop_avail_update,
	.mmap_commit = snd_pcm_dsnoop_mmap_commit,
};

/**
 * \brief Creates a new dsnoop PCM
 * \param pcmp Returns created PCM handle
 * \param name Name of PCM
 * \param ipc_key IPC key for semaphore and shared memory
 * \param ipc_perm IPC permissions for semaphore and shared memory
 * \param params Parameters for slave
 * \param bindings Channel bindings
 * \param slowptr Slow but more precise pointer updates
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
			key_t ipc_key, mode_t ipc_perm,
			struct slave_params *params,
			snd_config_t *bindings,
			int slowptr,
			snd_config_t *root, snd_config_t *sconf,
			snd_pcm_stream_t stream, int mode)
{
	snd_pcm_t *pcm = NULL, *spcm = NULL;
	snd_pcm_direct_t *dsnoop = NULL;
	int ret, first_instance, fail_sem_loop = 10;

	assert(pcmp);

	if (stream != SND_PCM_STREAM_CAPTURE) {
		SNDERR("The dsnoop plugin supports only capture stream");
		return -EINVAL;
	}

	dsnoop = calloc(1, sizeof(snd_pcm_direct_t));
	if (!dsnoop) {
		ret = -ENOMEM;
		goto _err;
	}
	
	ret = snd_pcm_direct_parse_bindings(dsnoop, bindings);
	if (ret < 0)
		goto _err;
	
	dsnoop->ipc_key = ipc_key;
	dsnoop->ipc_perm = ipc_perm;
	dsnoop->semid = -1;
	dsnoop->shmid = -1;

	ret = snd_pcm_new(&pcm, dsnoop->type = SND_PCM_TYPE_DSNOOP, name, stream, mode);
	if (ret < 0)
		goto _err;

	while (1) {
		ret = snd_pcm_direct_semaphore_create_or_connect(dsnoop);
		if (ret < 0) {
			SNDERR("unable to create IPC semaphore");
			goto _err;
		}
	
		ret = snd_pcm_direct_semaphore_down(dsnoop, DIRECT_IPC_SEM_CLIENT);
		if (ret < 0) {
			snd_pcm_direct_semaphore_discard(dsnoop);
			if (--fail_sem_loop <= 0)
				goto _err;
			continue;
		}
		break;
	}
		
	first_instance = ret = snd_pcm_direct_shm_create_or_connect(dsnoop);
	if (ret < 0) {
		SNDERR("unable to create IPC shm instance");
		goto _err;
	}
		
	pcm->ops = &snd_pcm_dsnoop_ops;
	pcm->fast_ops = &snd_pcm_dsnoop_fast_ops;
	pcm->private_data = dsnoop;
	dsnoop->state = SND_PCM_STATE_OPEN;
	dsnoop->slowptr = slowptr;

	if (first_instance) {
		ret = snd_pcm_open_slave(&spcm, root, sconf, stream, mode);
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
		
		ret = snd_pcm_direct_server_create(dsnoop);
		if (ret < 0) {
			SNDERR("unable to create server");
			goto _err;
		}

		dsnoop->shmptr->type = spcm->type;
	} else {
		ret = snd_pcm_direct_client_connect(dsnoop);
		if (ret < 0) {
			SNDERR("unable to connect client");
			return ret;
		}
			
		ret = snd_pcm_hw_open_fd(&spcm, "dsnoop_client", dsnoop->hw_fd, 0);
		if (ret < 0) {
			SNDERR("unable to open hardware");
			goto _err;
		}
		
		spcm->donot_close = 1;
		spcm->setup = 1;
		spcm->buffer_size = dsnoop->shmptr->s.buffer_size;
		spcm->sample_bits = dsnoop->shmptr->s.sample_bits;
		spcm->channels = dsnoop->shmptr->s.channels;
		spcm->format = dsnoop->shmptr->s.format;
		spcm->boundary = dsnoop->shmptr->s.boundary;
		spcm->info = dsnoop->shmptr->s.info;
		ret = snd_pcm_mmap(spcm);
		if (ret < 0) {
			SNDERR("unable to mmap channels");
			goto _err;
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
		
	pcm->mmap_rw = 1;
	snd_pcm_set_hw_ptr(pcm, &dsnoop->hw_ptr, -1, 0);
	snd_pcm_set_appl_ptr(pcm, &dsnoop->appl_ptr, -1, 0);
	
	if (dsnoop->channels == UINT_MAX)
		dsnoop->channels = dsnoop->shmptr->s.channels;
	
	snd_pcm_direct_semaphore_up(dsnoop, DIRECT_IPC_SEM_CLIENT);

	*pcmp = pcm;
	return 0;
	
 _err:
	if (dsnoop) {
	 	if (dsnoop->timer)
 			snd_timer_close(dsnoop->timer);
 		if (dsnoop->server)
 			snd_pcm_direct_server_discard(dsnoop);
 		if (dsnoop->client)
 			snd_pcm_direct_client_discard(dsnoop);
 		if (spcm)
 			snd_pcm_close(spcm);
 		if (dsnoop->shmid >= 0) {
 			if (snd_pcm_direct_shm_discard(dsnoop) > 0) {
			 	if (dsnoop->semid >= 0) {
 					if (snd_pcm_direct_semaphore_discard(dsnoop) < 0)
 						snd_pcm_direct_semaphore_up(dsnoop, DIRECT_IPC_SEM_CLIENT);
 				}
 			}
 		}
 		if (dsnoop->bindings)
 			free(dsnoop->bindings);
		free(dsnoop);
	}
	if (pcm)
		snd_pcm_free(pcm);
	return ret;
}

/*! \page pcm_plugins

\section pcm_plugins_snoop Plugin: dsnoop

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
		period_size INT	# in bytes
		buffer_time INT	# in usec
		# or
		buffer_size INT # in bytes
		periods INT	# when buffer_size or buffer_time is not specified
	}
	bindings {		# note: this is client independent!!!
		N INT		# maps slave channel to client channel N
	}
}
\endcode

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
	snd_config_iterator_t i, next;
	snd_config_t *slave = NULL, *bindings = NULL, *sconf;
	struct slave_params params;
	int bsize, psize, ipc_key_add_uid = 0, slowptr = 0;
	key_t ipc_key = 0;
	mode_t ipc_perm = 0600;
	int err;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (snd_pcm_conf_generic_id(id))
			continue;
		if (strcmp(id, "ipc_key") == 0) {
			long key;
			err = snd_config_get_integer(n, &key);
			if (err < 0) {
				SNDERR("The field ipc_key must be an integer type");
				return err;
			}
			ipc_key = key;
			continue;
		}
		if (strcmp(id, "ipc_perm") == 0) {
			char *perm;
			char *endp;
			err = snd_config_get_ascii(n, &perm);
			if (err < 0) {
				SNDERR("The field ipc_perm must be a valid file permission");
				return err;
			}
			if (isdigit(*perm) == 0) {
				SNDERR("The field ipc_perm must be a valid file permission");
				return -EINVAL;
			}
			ipc_perm = strtol(perm, &endp, 8);
			continue;
		}
		if (strcmp(id, "ipc_key_add_uid") == 0) {
			char *tmp;
			err = snd_config_get_ascii(n, &tmp);
			if (err < 0) {
				SNDERR("The field ipc_key_add_uid must be a boolean type");
				return err;
			}
			err = snd_config_get_bool_ascii(tmp);
			free(tmp);
			if (err < 0) {
				SNDERR("The field ipc_key_add_uid must be a boolean type");
				return err;
			}
			ipc_key_add_uid = err;
			continue;
		}
		if (strcmp(id, "slave") == 0) {
			slave = n;
			continue;
		}
		if (strcmp(id, "bindings") == 0) {
			bindings = n;
			continue;
		}
		if (strcmp(id, "slowptr") == 0) {
			char *tmp;
			err = snd_config_get_ascii(n, &tmp);
			if (err < 0) {
				SNDERR("The field slowptr must be a boolean type");
				return err;
			}
			err = snd_config_get_bool_ascii(tmp);
			free(tmp);
			if (err < 0) {
				SNDERR("The field slowptr must be a boolean type");
				return err;
			}
			slowptr = err;
			continue;
		}
		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}
	if (!slave) {
		SNDERR("slave is not defined");
		return -EINVAL;
	}
	if (ipc_key_add_uid)
		ipc_key += getuid();
	if (!ipc_key) {
		SNDERR("Unique IPC key is not defined");
		return -EINVAL;
	}
	/* the default settings, it might be invalid for some hardware */
	params.format = SND_PCM_FORMAT_S16;
	params.rate = 48000;
	params.channels = 2;
	params.period_time = 125000;	/* 0.125 seconds */
	params.buffer_time = -1;
	bsize = psize = -1;
	params.periods = 3;
	err = snd_pcm_slave_conf(root, slave, &sconf, 8,
				 SND_PCM_HW_PARAM_FORMAT, 0, &params.format,
				 SND_PCM_HW_PARAM_RATE, 0, &params.rate,
				 SND_PCM_HW_PARAM_CHANNELS, 0, &params.channels,
				 SND_PCM_HW_PARAM_PERIOD_TIME, 0, &params.period_time,
				 SND_PCM_HW_PARAM_BUFFER_TIME, 0, &params.buffer_time,
				 SND_PCM_HW_PARAM_PERIOD_SIZE, 0, &psize,
				 SND_PCM_HW_PARAM_BUFFER_SIZE, 0, &bsize,
				 SND_PCM_HW_PARAM_PERIODS, 0, &params.periods);
	if (err < 0)
		return err;

	/* sorry, limited features */
	if (params.format != SND_PCM_FORMAT_S16 &&
	    params.format != SND_PCM_FORMAT_S32) {
		SNDERR("invalid format, specify s16 or s32");
		snd_config_delete(sconf);
		return -EINVAL;
	}

	params.period_size = psize;
	params.buffer_size = bsize;
	err = snd_pcm_dsnoop_open(pcmp, name, ipc_key, ipc_perm, &params, bindings, slowptr, root, sconf, stream, mode);
	if (err < 0)
		snd_config_delete(sconf);
	return err;
}
#ifndef DOC_HIDDEN
SND_DLSYM_BUILD_VERSION(_snd_pcm_dsnoop_open, SND_PCM_DLSYM_VERSION);
#endif
