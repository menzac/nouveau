/*
 * Copyright 2012 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */
#include "nv04.h"

#include <core/client.h>
#include <core/engctx.h>
#include <core/handle.h>
#include <core/ramht.h>
#include <subdev/instmem.h>
#include <subdev/timer.h>
#include <engine/sw.h>

#include <nvif/class.h>
#include <nvif/unpack.h>

static struct ramfc_desc
nv04_ramfc[] = {
	{ 32,  0, 0x00,  0, NV04_PFIFO_CACHE1_DMA_PUT },
	{ 32,  0, 0x04,  0, NV04_PFIFO_CACHE1_DMA_GET },
	{ 16,  0, 0x08,  0, NV04_PFIFO_CACHE1_DMA_INSTANCE },
	{ 16, 16, 0x08,  0, NV04_PFIFO_CACHE1_DMA_DCOUNT },
	{ 32,  0, 0x0c,  0, NV04_PFIFO_CACHE1_DMA_STATE },
	{ 32,  0, 0x10,  0, NV04_PFIFO_CACHE1_DMA_FETCH },
	{ 32,  0, 0x14,  0, NV04_PFIFO_CACHE1_ENGINE },
	{ 32,  0, 0x18,  0, NV04_PFIFO_CACHE1_PULL1 },
	{}
};

/*******************************************************************************
 * FIFO channel objects
 ******************************************************************************/

int
nv04_fifo_object_attach(struct nvkm_object *parent,
			struct nvkm_object *object, u32 handle)
{
	struct nv04_fifo *fifo = (void *)parent->engine;
	struct nv04_fifo_chan *chan = (void *)parent;
	struct nvkm_instmem *imem = fifo->base.engine.subdev.device->imem;
	u32 context, chid = chan->base.chid;
	int ret;

	if (nv_iclass(object, NV_GPUOBJ_CLASS))
		context = nv_gpuobj(object)->addr >> 4;
	else
		context = 0x00000004; /* just non-zero */

	if (object->engine) {
		switch (nv_engidx(object->engine)) {
		case NVDEV_ENGINE_DMAOBJ:
		case NVDEV_ENGINE_SW:
			context |= 0x00000000;
			break;
		case NVDEV_ENGINE_GR:
			context |= 0x00010000;
			break;
		case NVDEV_ENGINE_MPEG:
			context |= 0x00020000;
			break;
		default:
			return -EINVAL;
		}
	}

	context |= 0x80000000; /* valid */
	context |= chid << 24;

	mutex_lock(&nv_subdev(fifo)->mutex);
	ret = nvkm_ramht_insert(imem->ramht, NULL, chid, 0, handle, context);
	mutex_unlock(&nv_subdev(fifo)->mutex);
	return ret;
}

void
nv04_fifo_object_detach(struct nvkm_object *parent, int cookie)
{
	struct nv04_fifo *fifo = (void *)parent->engine;
	struct nvkm_instmem *imem = fifo->base.engine.subdev.device->imem;
	mutex_lock(&nv_subdev(fifo)->mutex);
	nvkm_ramht_remove(imem->ramht, cookie);
	mutex_unlock(&nv_subdev(fifo)->mutex);
}

int
nv04_fifo_context_attach(struct nvkm_object *parent,
			 struct nvkm_object *object)
{
	nv_engctx(object)->addr = nvkm_fifo_chan(parent)->chid;
	return 0;
}

static int
nv04_fifo_chan_ctor(struct nvkm_object *parent,
		    struct nvkm_object *engine,
		    struct nvkm_oclass *oclass, void *data, u32 size,
		    struct nvkm_object **pobject)
{
	union {
		struct nv03_channel_dma_v0 v0;
	} *args = data;
	struct nv04_fifo *fifo = (void *)engine;
	struct nvkm_instmem *imem = fifo->base.engine.subdev.device->imem;
	struct nv04_fifo_chan *chan;
	int ret;

	nvif_ioctl(parent, "create channel dma size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nvif_ioctl(parent, "create channel dma vers %d pushbuf %llx "
				   "offset %08x\n", args->v0.version,
			   args->v0.pushbuf, args->v0.offset);
	} else
		return ret;

	ret = nvkm_fifo_channel_create(parent, engine, oclass, 0, 0x800000,
				       0x10000, args->v0.pushbuf,
				       (1ULL << NVDEV_ENGINE_DMAOBJ) |
				       (1ULL << NVDEV_ENGINE_SW) |
				       (1ULL << NVDEV_ENGINE_GR), &chan);
	*pobject = nv_object(chan);
	if (ret)
		return ret;

	args->v0.chid = chan->base.chid;

	nv_parent(chan)->object_attach = nv04_fifo_object_attach;
	nv_parent(chan)->object_detach = nv04_fifo_object_detach;
	nv_parent(chan)->context_attach = nv04_fifo_context_attach;
	chan->ramfc = chan->base.chid * 32;

	nvkm_kmap(imem->ramfc);
	nvkm_wo32(imem->ramfc, chan->ramfc + 0x00, args->v0.offset);
	nvkm_wo32(imem->ramfc, chan->ramfc + 0x04, args->v0.offset);
	nvkm_wo32(imem->ramfc, chan->ramfc + 0x08, chan->base.pushgpu->addr >> 4);
	nvkm_wo32(imem->ramfc, chan->ramfc + 0x10,
			     NV_PFIFO_CACHE1_DMA_FETCH_TRIG_128_BYTES |
			     NV_PFIFO_CACHE1_DMA_FETCH_SIZE_128_BYTES |
#ifdef __BIG_ENDIAN
			     NV_PFIFO_CACHE1_BIG_ENDIAN |
#endif
			     NV_PFIFO_CACHE1_DMA_FETCH_MAX_REQS_8);
	nvkm_done(imem->ramfc);
	return 0;
}

void
nv04_fifo_chan_dtor(struct nvkm_object *object)
{
	struct nv04_fifo *fifo = (void *)object->engine;
	struct nv04_fifo_chan *chan = (void *)object;
	struct nvkm_instmem *imem = fifo->base.engine.subdev.device->imem;
	struct ramfc_desc *c = fifo->ramfc_desc;

	nvkm_kmap(imem->ramfc);
	do {
		nvkm_wo32(imem->ramfc, chan->ramfc + c->ctxp, 0x00000000);
	} while ((++c)->bits);
	nvkm_done(imem->ramfc);

	nvkm_fifo_channel_destroy(&chan->base);
}

int
nv04_fifo_chan_init(struct nvkm_object *object)
{
	struct nv04_fifo *fifo = (void *)object->engine;
	struct nv04_fifo_chan *chan = (void *)object;
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	u32 mask = 1 << chan->base.chid;
	unsigned long flags;
	int ret;

	ret = nvkm_fifo_channel_init(&chan->base);
	if (ret)
		return ret;

	spin_lock_irqsave(&fifo->base.lock, flags);
	nvkm_mask(device, NV04_PFIFO_MODE, mask, mask);
	spin_unlock_irqrestore(&fifo->base.lock, flags);
	return 0;
}

int
nv04_fifo_chan_fini(struct nvkm_object *object, bool suspend)
{
	struct nv04_fifo *fifo = (void *)object->engine;
	struct nv04_fifo_chan *chan = (void *)object;
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	struct nvkm_memory *fctx = device->imem->ramfc;
	struct ramfc_desc *c;
	unsigned long flags;
	u32 data = chan->ramfc;
	u32 chid;

	/* prevent fifo context switches */
	spin_lock_irqsave(&fifo->base.lock, flags);
	nvkm_wr32(device, NV03_PFIFO_CACHES, 0);

	/* if this channel is active, replace it with a null context */
	chid = nvkm_rd32(device, NV03_PFIFO_CACHE1_PUSH1) & fifo->base.max;
	if (chid == chan->base.chid) {
		nvkm_mask(device, NV04_PFIFO_CACHE1_DMA_PUSH, 0x00000001, 0);
		nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH0, 0);
		nvkm_mask(device, NV04_PFIFO_CACHE1_PULL0, 0x00000001, 0);

		c = fifo->ramfc_desc;
		do {
			u32 rm = ((1ULL << c->bits) - 1) << c->regs;
			u32 cm = ((1ULL << c->bits) - 1) << c->ctxs;
			u32 rv = (nvkm_rd32(device, c->regp) &  rm) >> c->regs;
			u32 cv = (nvkm_ro32(fctx, c->ctxp + data) & ~cm);
			nvkm_wo32(fctx, c->ctxp + data, cv | (rv << c->ctxs));
		} while ((++c)->bits);

		c = fifo->ramfc_desc;
		do {
			nvkm_wr32(device, c->regp, 0x00000000);
		} while ((++c)->bits);

		nvkm_wr32(device, NV03_PFIFO_CACHE1_GET, 0);
		nvkm_wr32(device, NV03_PFIFO_CACHE1_PUT, 0);
		nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH1, fifo->base.max);
		nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH0, 1);
		nvkm_wr32(device, NV04_PFIFO_CACHE1_PULL0, 1);
	}

	/* restore normal operation, after disabling dma mode */
	nvkm_mask(device, NV04_PFIFO_MODE, 1 << chan->base.chid, 0);
	nvkm_wr32(device, NV03_PFIFO_CACHES, 1);
	spin_unlock_irqrestore(&fifo->base.lock, flags);

	return nvkm_fifo_channel_fini(&chan->base, suspend);
}

static struct nvkm_ofuncs
nv04_fifo_ofuncs = {
	.ctor = nv04_fifo_chan_ctor,
	.dtor = nv04_fifo_chan_dtor,
	.init = nv04_fifo_chan_init,
	.fini = nv04_fifo_chan_fini,
	.map  = _nvkm_fifo_channel_map,
	.rd32 = _nvkm_fifo_channel_rd32,
	.wr32 = _nvkm_fifo_channel_wr32,
	.ntfy = _nvkm_fifo_channel_ntfy
};

static struct nvkm_oclass
nv04_fifo_sclass[] = {
	{ NV03_CHANNEL_DMA, &nv04_fifo_ofuncs },
	{}
};

/*******************************************************************************
 * FIFO context - basically just the instmem reserved for the channel
 ******************************************************************************/

int
nv04_fifo_context_ctor(struct nvkm_object *parent,
		       struct nvkm_object *engine,
		       struct nvkm_oclass *oclass, void *data, u32 size,
		       struct nvkm_object **pobject)
{
	struct nv04_fifo_base *base;
	int ret;

	ret = nvkm_fifo_context_create(parent, engine, oclass, NULL, 0x1000,
				       0x1000, NVOBJ_FLAG_HEAP, &base);
	*pobject = nv_object(base);
	if (ret)
		return ret;

	return 0;
}

static struct nvkm_oclass
nv04_fifo_cclass = {
	.handle = NV_ENGCTX(FIFO, 0x04),
	.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = nv04_fifo_context_ctor,
		.dtor = _nvkm_fifo_context_dtor,
		.init = _nvkm_fifo_context_init,
		.fini = _nvkm_fifo_context_fini,
		.rd32 = _nvkm_fifo_context_rd32,
		.wr32 = _nvkm_fifo_context_wr32,
	},
};

/*******************************************************************************
 * PFIFO engine
 ******************************************************************************/

void
nv04_fifo_pause(struct nvkm_fifo *obj, unsigned long *pflags)
__acquires(fifo->base.lock)
{
	struct nv04_fifo *fifo = container_of(obj, typeof(*fifo), base);
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	unsigned long flags;

	spin_lock_irqsave(&fifo->base.lock, flags);
	*pflags = flags;

	nvkm_wr32(device, NV03_PFIFO_CACHES, 0x00000000);
	nvkm_mask(device, NV04_PFIFO_CACHE1_PULL0, 0x00000001, 0x00000000);

	/* in some cases the puller may be left in an inconsistent state
	 * if you try to stop it while it's busy translating handles.
	 * sometimes you get a CACHE_ERROR, sometimes it just fails
	 * silently; sending incorrect instance offsets to PGRAPH after
	 * it's started up again.
	 *
	 * to avoid this, we invalidate the most recently calculated
	 * instance.
	 */
	nvkm_msec(device, 2000,
		u32 tmp = nvkm_rd32(device, NV04_PFIFO_CACHE1_PULL0);
		if (!(tmp & NV04_PFIFO_CACHE1_PULL0_HASH_BUSY))
			break;
	);

	if (nvkm_rd32(device, NV04_PFIFO_CACHE1_PULL0) &
			  NV04_PFIFO_CACHE1_PULL0_HASH_FAILED)
		nvkm_wr32(device, NV03_PFIFO_INTR_0, NV_PFIFO_INTR_CACHE_ERROR);

	nvkm_wr32(device, NV04_PFIFO_CACHE1_HASH, 0x00000000);
}

void
nv04_fifo_start(struct nvkm_fifo *obj, unsigned long *pflags)
__releases(fifo->base.lock)
{
	struct nv04_fifo *fifo = container_of(obj, typeof(*fifo), base);
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	unsigned long flags = *pflags;

	nvkm_mask(device, NV04_PFIFO_CACHE1_PULL0, 0x00000001, 0x00000001);
	nvkm_wr32(device, NV03_PFIFO_CACHES, 0x00000001);

	spin_unlock_irqrestore(&fifo->base.lock, flags);
}

static const char *
nv_dma_state_err(u32 state)
{
	static const char * const desc[] = {
		"NONE", "CALL_SUBR_ACTIVE", "INVALID_MTHD", "RET_SUBR_INACTIVE",
		"INVALID_CMD", "IB_EMPTY"/* NV50+ */, "MEM_FAULT", "UNK"
	};
	return desc[(state >> 29) & 0x7];
}

static bool
nv04_fifo_swmthd(struct nvkm_device *device, u32 chid, u32 addr, u32 data)
{
	struct nvkm_sw *sw = device->sw;
	const int subc = (addr & 0x0000e000) >> 13;
	const int mthd = (addr & 0x00001ffc);
	const u32 mask = 0x0000000f << (subc * 4);
	u32 engine = nvkm_rd32(device, 0x003280);
	bool handled = false;

	switch (mthd) {
	case 0x0000 ... 0x0000: /* subchannel's engine -> software */
		nvkm_wr32(device, 0x003280, (engine &= ~mask));
	case 0x0180 ... 0x01fc: /* handle -> instance */
		data = nvkm_rd32(device, 0x003258) & 0x0000ffff;
	case 0x0100 ... 0x017c:
	case 0x0200 ... 0x1ffc: /* pass method down to sw */
		if (!(engine & mask) && sw)
			handled = nvkm_sw_mthd(sw, chid, subc, mthd, data);
		break;
	default:
		break;
	}

	return handled;
}

static void
nv04_fifo_cache_error(struct nv04_fifo *fifo, u32 chid, u32 get)
{
	struct nvkm_subdev *subdev = &fifo->base.engine.subdev;
	struct nvkm_device *device = subdev->device;
	u32 pull0 = nvkm_rd32(device, 0x003250);
	u32 mthd, data;
	int ptr;

	/* NV_PFIFO_CACHE1_GET actually goes to 0xffc before wrapping on my
	 * G80 chips, but CACHE1 isn't big enough for this much data.. Tests
	 * show that it wraps around to the start at GET=0x800.. No clue as to
	 * why..
	 */
	ptr = (get & 0x7ff) >> 2;

	if (device->card_type < NV_40) {
		mthd = nvkm_rd32(device, NV04_PFIFO_CACHE1_METHOD(ptr));
		data = nvkm_rd32(device, NV04_PFIFO_CACHE1_DATA(ptr));
	} else {
		mthd = nvkm_rd32(device, NV40_PFIFO_CACHE1_METHOD(ptr));
		data = nvkm_rd32(device, NV40_PFIFO_CACHE1_DATA(ptr));
	}

	if (!(pull0 & 0x00000100) ||
	    !nv04_fifo_swmthd(device, chid, mthd, data)) {
		const char *client_name =
			nvkm_client_name_for_fifo_chid(&fifo->base, chid);
		nvkm_error(subdev, "CACHE_ERROR - "
			   "ch %d [%s] subc %d mthd %04x data %08x\n",
			   chid, client_name, (mthd >> 13) & 7, mthd & 0x1ffc,
			   data);
	}

	nvkm_wr32(device, NV04_PFIFO_CACHE1_DMA_PUSH, 0);
	nvkm_wr32(device, NV03_PFIFO_INTR_0, NV_PFIFO_INTR_CACHE_ERROR);

	nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH0,
		nvkm_rd32(device, NV03_PFIFO_CACHE1_PUSH0) & ~1);
	nvkm_wr32(device, NV03_PFIFO_CACHE1_GET, get + 4);
	nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH0,
		nvkm_rd32(device, NV03_PFIFO_CACHE1_PUSH0) | 1);
	nvkm_wr32(device, NV04_PFIFO_CACHE1_HASH, 0);

	nvkm_wr32(device, NV04_PFIFO_CACHE1_DMA_PUSH,
		nvkm_rd32(device, NV04_PFIFO_CACHE1_DMA_PUSH) | 1);
	nvkm_wr32(device, NV04_PFIFO_CACHE1_PULL0, 1);
}

static void
nv04_fifo_dma_pusher(struct nv04_fifo *fifo, u32 chid)
{
	struct nvkm_subdev *subdev = &fifo->base.engine.subdev;
	struct nvkm_device *device = subdev->device;
	u32 dma_get = nvkm_rd32(device, 0x003244);
	u32 dma_put = nvkm_rd32(device, 0x003240);
	u32 push = nvkm_rd32(device, 0x003220);
	u32 state = nvkm_rd32(device, 0x003228);
	const char *client_name;

	client_name = nvkm_client_name_for_fifo_chid(&fifo->base, chid);

	if (device->card_type == NV_50) {
		u32 ho_get = nvkm_rd32(device, 0x003328);
		u32 ho_put = nvkm_rd32(device, 0x003320);
		u32 ib_get = nvkm_rd32(device, 0x003334);
		u32 ib_put = nvkm_rd32(device, 0x003330);

		nvkm_error(subdev, "DMA_PUSHER - "
			   "ch %d [%s] get %02x%08x put %02x%08x ib_get %08x "
			   "ib_put %08x state %08x (err: %s) push %08x\n",
			   chid, client_name, ho_get, dma_get, ho_put, dma_put,
			   ib_get, ib_put, state, nv_dma_state_err(state),
			   push);

		/* METHOD_COUNT, in DMA_STATE on earlier chipsets */
		nvkm_wr32(device, 0x003364, 0x00000000);
		if (dma_get != dma_put || ho_get != ho_put) {
			nvkm_wr32(device, 0x003244, dma_put);
			nvkm_wr32(device, 0x003328, ho_put);
		} else
		if (ib_get != ib_put)
			nvkm_wr32(device, 0x003334, ib_put);
	} else {
		nvkm_error(subdev, "DMA_PUSHER - ch %d [%s] get %08x put %08x "
				   "state %08x (err: %s) push %08x\n",
			   chid, client_name, dma_get, dma_put, state,
			   nv_dma_state_err(state), push);

		if (dma_get != dma_put)
			nvkm_wr32(device, 0x003244, dma_put);
	}

	nvkm_wr32(device, 0x003228, 0x00000000);
	nvkm_wr32(device, 0x003220, 0x00000001);
	nvkm_wr32(device, 0x002100, NV_PFIFO_INTR_DMA_PUSHER);
}

void
nv04_fifo_intr(struct nvkm_subdev *subdev)
{
	struct nvkm_device *device = subdev->device;
	struct nv04_fifo *fifo = (void *)subdev;
	u32 mask = nvkm_rd32(device, NV03_PFIFO_INTR_EN_0);
	u32 stat = nvkm_rd32(device, NV03_PFIFO_INTR_0) & mask;
	u32 reassign, chid, get, sem;

	reassign = nvkm_rd32(device, NV03_PFIFO_CACHES) & 1;
	nvkm_wr32(device, NV03_PFIFO_CACHES, 0);

	chid = nvkm_rd32(device, NV03_PFIFO_CACHE1_PUSH1) & fifo->base.max;
	get  = nvkm_rd32(device, NV03_PFIFO_CACHE1_GET);

	if (stat & NV_PFIFO_INTR_CACHE_ERROR) {
		nv04_fifo_cache_error(fifo, chid, get);
		stat &= ~NV_PFIFO_INTR_CACHE_ERROR;
	}

	if (stat & NV_PFIFO_INTR_DMA_PUSHER) {
		nv04_fifo_dma_pusher(fifo, chid);
		stat &= ~NV_PFIFO_INTR_DMA_PUSHER;
	}

	if (stat & NV_PFIFO_INTR_SEMAPHORE) {
		stat &= ~NV_PFIFO_INTR_SEMAPHORE;
		nvkm_wr32(device, NV03_PFIFO_INTR_0, NV_PFIFO_INTR_SEMAPHORE);

		sem = nvkm_rd32(device, NV10_PFIFO_CACHE1_SEMAPHORE);
		nvkm_wr32(device, NV10_PFIFO_CACHE1_SEMAPHORE, sem | 0x1);

		nvkm_wr32(device, NV03_PFIFO_CACHE1_GET, get + 4);
		nvkm_wr32(device, NV04_PFIFO_CACHE1_PULL0, 1);
	}

	if (device->card_type == NV_50) {
		if (stat & 0x00000010) {
			stat &= ~0x00000010;
			nvkm_wr32(device, 0x002100, 0x00000010);
		}

		if (stat & 0x40000000) {
			nvkm_wr32(device, 0x002100, 0x40000000);
			nvkm_fifo_uevent(&fifo->base);
			stat &= ~0x40000000;
		}
	}

	if (stat) {
		nvkm_warn(subdev, "intr %08x\n", stat);
		nvkm_mask(device, NV03_PFIFO_INTR_EN_0, stat, 0x00000000);
		nvkm_wr32(device, NV03_PFIFO_INTR_0, stat);
	}

	nvkm_wr32(device, NV03_PFIFO_CACHES, reassign);
}

static int
nv04_fifo_ctor(struct nvkm_object *parent, struct nvkm_object *engine,
	       struct nvkm_oclass *oclass, void *data, u32 size,
	       struct nvkm_object **pobject)
{
	struct nv04_fifo *fifo;
	int ret;

	ret = nvkm_fifo_create(parent, engine, oclass, 0, 15, &fifo);
	*pobject = nv_object(fifo);
	if (ret)
		return ret;

	nv_subdev(fifo)->unit = 0x00000100;
	nv_subdev(fifo)->intr = nv04_fifo_intr;
	nv_engine(fifo)->cclass = &nv04_fifo_cclass;
	nv_engine(fifo)->sclass = nv04_fifo_sclass;
	fifo->base.pause = nv04_fifo_pause;
	fifo->base.start = nv04_fifo_start;
	fifo->ramfc_desc = nv04_ramfc;
	return 0;
}

void
nv04_fifo_dtor(struct nvkm_object *object)
{
	struct nv04_fifo *fifo = (void *)object;
	nvkm_fifo_destroy(&fifo->base);
}

int
nv04_fifo_init(struct nvkm_object *object)
{
	struct nv04_fifo *fifo = (void *)object;
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	struct nvkm_instmem *imem = device->imem;
	struct nvkm_ramht *ramht = imem->ramht;
	struct nvkm_memory *ramro = imem->ramro;
	struct nvkm_memory *ramfc = imem->ramfc;
	int ret;

	ret = nvkm_fifo_init(&fifo->base);
	if (ret)
		return ret;

	nvkm_wr32(device, NV04_PFIFO_DELAY_0, 0x000000ff);
	nvkm_wr32(device, NV04_PFIFO_DMA_TIMESLICE, 0x0101ffff);

	nvkm_wr32(device, NV03_PFIFO_RAMHT, (0x03 << 24) /* search 128 */ |
					    ((ramht->bits - 9) << 16) |
					    (ramht->gpuobj->addr >> 8));
	nvkm_wr32(device, NV03_PFIFO_RAMRO, nvkm_memory_addr(ramro) >> 8);
	nvkm_wr32(device, NV03_PFIFO_RAMFC, nvkm_memory_addr(ramfc) >> 8);

	nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH1, fifo->base.max);

	nvkm_wr32(device, NV03_PFIFO_INTR_0, 0xffffffff);
	nvkm_wr32(device, NV03_PFIFO_INTR_EN_0, 0xffffffff);

	nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH0, 1);
	nvkm_wr32(device, NV04_PFIFO_CACHE1_PULL0, 1);
	nvkm_wr32(device, NV03_PFIFO_CACHES, 1);
	return 0;
}

struct nvkm_oclass *
nv04_fifo_oclass = &(struct nvkm_oclass) {
	.handle = NV_ENGINE(FIFO, 0x04),
	.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = nv04_fifo_ctor,
		.dtor = nv04_fifo_dtor,
		.init = nv04_fifo_init,
		.fini = _nvkm_fifo_fini,
	},
};
