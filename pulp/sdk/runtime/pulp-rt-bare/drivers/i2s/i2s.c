/*
 * Copyright (C) 2018 ETH Zurich and University of Bologna
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Copyright (C) 2018 GreenWaves Technologies
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * Authors: Germain Haugou, ETH (germain.haugou@iis.ee.ethz.ch)
 */

#include "rt/rt_api.h"

rt_i2s_dev_t i2s_desc;
static rt_i2s_t *__rt_i2s_first[ARCHI_UDMA_NB_I2S];

static inline int __rt_i2s_id(rt_i2s_t *i2s)
{
  return ARCHI_UDMA_I2S_ID(i2s->i2s_id) - ARCHI_UDMA_I2S_ID(0);
}

void rt_i2s_conf_init(rt_i2s_conf_t *conf)
{
  conf->frequency = 44100;
  conf->decimation_log2 = 8;
  conf->pdm = 1;
  conf->dual = 0;
  conf->width = 16;
  conf->id = -1;
}

static void __rt_i2s_free(rt_i2s_t *i2s)
{
  if (i2s != NULL) {
    rt_free(RT_ALLOC_FC_DATA, (void *)i2s, sizeof(rt_i2s_t));
  }
}

static rt_i2s_t *__rt_i2s_open(rt_dev_t *dev, rt_i2s_conf_t *conf, rt_event_t*event)
{
  rt_i2s_t *i2s = NULL;
  int periph_id, sub_periph_id;

  if (dev)
  {
    periph_id = (dev->channel & 0xf);
    sub_periph_id = (dev->channel >> 4) & 0xf;
  }
  else
  {
    periph_id = (conf->id >> 1) + ARCHI_UDMA_I2S_ID(0);
    sub_periph_id = conf->id & 1;
  }

  // Check parameters
  if (conf->width != 16) return NULL;


  int periph_freq = __rt_freq_periph_get();

  // The actual I2S frequency is the sampling one multiplied by the decimation
  // as we get one 1 decimiation bit per cycle.
  int i2s_freq = conf->frequency;
  if (conf->pdm) i2s_freq *= (1<<conf->decimation_log2);
  else i2s_freq *= conf->width;

  if (periph_freq < i2s_freq*2) {
    return NULL;
  }

  i2s = rt_alloc(RT_ALLOC_FC_DATA, sizeof(rt_i2s_t));
  if (i2s == NULL) goto error;

  // Remember the desired frequency to update the I2S divider in case the soc frequency is updated
  i2s->i2s_freq = i2s_freq;
  i2s->pdm = conf->pdm;
  i2s->decimation_log2 = conf->decimation_log2;

  i2s->periph_id = periph_id;
  i2s->udma_channel = i2s->periph_id*2;
  i2s->i2s_id = i2s->periph_id - ARCHI_UDMA_I2S_ID(0);
  i2s->clk = sub_periph_id;
  i2s->dual = conf->dual;
  i2s->width = conf->width;
  i2s->running = 0;

  // Now we have checked possible errors, register the device and activate udma i2s in case
  // it is the first opened
  if (__rt_i2s_first[i2s->i2s_id] == NULL) {
    plp_udma_cg_set(plp_udma_cg_get() | (1<<ARCHI_UDMA_I2S_ID(i2s->periph_id)));
  }
  i2s->next = __rt_i2s_first[i2s->i2s_id];
  __rt_i2s_first[i2s->i2s_id] = i2s;

  // Now configure the I2s part

  soc_eu_fcEventMask_setEvent(i2s->udma_channel + i2s->clk);

  int shift = 10 - i2s->decimation_log2;
  if (shift > 7) shift = 7;

  hal_i2s_filt_ch_set(i2s->i2s_id, i2s->clk, (shift << 16) | ((1<<i2s->decimation_log2)-1));

  unsigned int mode = hal_i2s_chmode_get(i2s->i2s_id);
  int clk = i2s->clk;

  mode &= ~(I2S_CHMODE_CH_SNAPCAM_MASK(clk) | I2S_CHMODE_CH_LSBFIRST_MASK(clk) | 
    I2S_CHMODE_CH_PDM_USEFILTER_MASK(clk) | I2S_CHMODE_CH_PDM_EN_MASK(clk) | 
    I2S_CHMODE_CH_USEDDR_MASK(clk) | I2S_CHMODE_CH_MODE_MASK(clk));

  mode |= I2S_CHMODE_CH_LSBFIRST_DIS(clk) | I2S_CHMODE_CH_MODE_CLK(clk,clk) ;

  if (i2s->dual) {
    mode |= I2S_CHMODE_CH_USEDDR_ENA(clk);
  }

  if (i2s->pdm) {
    mode |= I2S_CHMODE_CH_PDM_USEFILTER_ENA(clk) | I2S_CHMODE_CH_PDM_EN_ENA(clk);
  }

  hal_i2s_chmode_set(i2s->i2s_id, mode);

  return i2s;

error:
  __rt_i2s_free(i2s);
  return NULL;
}

static void __rt_i2s_close(rt_i2s_t *dev, rt_event_t*event)
{
  int i2s_id = __rt_i2s_id(dev);
  rt_i2s_t *current=__rt_i2s_first[i2s_id], *prev=NULL;

  while (current && current != dev)
  {
    prev = current;
    current = current->next;
  }

  if (prev) prev->next = dev->next;
  else __rt_i2s_first[i2s_id] = dev->next;

  // In case this was the last device using I2S, deactivate I2S
  if (__rt_i2s_first[i2s_id] == NULL)
  {
    plp_udma_cg_set(plp_udma_cg_get() & ~(1<<ARCHI_UDMA_I2S_ID(dev->i2s_id)));
  }
}

static inline void __rt_i2s_suspend(rt_i2s_t *dev)
{
  hal_i2s_cfg_clkgen_set(dev->i2s_id, dev->clk, 0);
}

static inline void __rt_i2s_resume(rt_i2s_t *dev)
{
  int periph_freq = __rt_freq_periph_get();
  int div = periph_freq / dev->i2s_freq;

  rt_trace(RT_TRACE_CAM, "[I2S] Resuming (i2s: %d, clk: %d, periph_freq: %d, i2s_freq: %d, div: %d)\n", dev->i2s_id, dev->clk, periph_freq, dev->i2s_freq, div);
  
  unsigned int conf = 
    I2S_CFG_CLKGEN_BITS_WORD(dev->width) | 
    I2S_CFG_CLKGEN_CLK_EN |
    I2S_CFG_CLKGEN0_CLKDIV((div-1)>>1);

  hal_i2s_cfg_clkgen_set(dev->i2s_id, dev->clk, conf);
}

void __rt_i2s_start(rt_i2s_t *dev)
{
  int irq = rt_irq_disable();
  rt_trace(RT_TRACE_CAM, "[I2S] Start (i2s: %d, clk: %d)\n", dev->i2s_id, dev->clk);
  dev->running = 1;
  __rt_i2s_resume(dev);
  rt_irq_restore(irq);
}

void __rt_i2s_stop(rt_i2s_t *dev)
{
  int irq = rt_irq_disable();
  rt_trace(RT_TRACE_CAM, "[I2S] Stop (i2s: %d, clk: %d)\n", dev->i2s_id, dev->clk);
  dev->running = 0;
  __rt_i2s_suspend(dev);
  rt_irq_restore(irq);
}

static int __rt_i2s_setfreq_before(void *arg)
{
  // The peripheral clock frequency is being changed, suspend all running devices
  for (int i=0; i<ARCHI_UDMA_NB_I2S; i++)
  {
    rt_i2s_t *current = __rt_i2s_first[i];
    while(current)
    {
      // This check is to make sure we don't start a device which has been stopped by the user
      if (current->running) __rt_i2s_suspend(current);
      current = current->next;
    }
  }
  return 0;
}

static int __rt_i2s_setfreq_after(void *arg)
{
  // The peripheral clock frequency has been changed, resume all running devices
  for (int i=0; i<ARCHI_UDMA_NB_I2S; i++)
  {
    rt_i2s_t *current = __rt_i2s_first[i];
    while(current)
    {
      if (current->running) __rt_i2s_resume(current);
      current = current->next;
    }
  }
  return 0;  
}


static void __rt_i2s_capture(rt_i2s_t *dev, void *buffer, size_t size, rt_event_t *event)
{
  rt_trace(RT_TRACE_CAM, "[I2S] Capture (buffer: %p, size: 0x%x)\n", buffer, size);

  int irq = rt_irq_disable();

  rt_event_t *call_event = __rt_wait_event_prepare(event);

  rt_periph_copy_init(&call_event->copy, 0);

  rt_periph_copy(&call_event->copy, dev->udma_channel + dev->clk, (unsigned int) buffer, size, UDMA_CHANNEL_CFG_SIZE_16, call_event);

  __rt_wait_event_check(event, call_event);

  rt_irq_restore(irq);
}

RT_FC_BOOT_CODE void __attribute__((constructor)) __rt_i2s_init()
{
  // In case the peripheral clock can dynamically change, we need to be notified
  // when this happens so that the I2S channels are stopped and resumed
  int err = 0;

  err |= __rt_cbsys_add(RT_CBSYS_PERIPH_SETFREQ_BEFORE, __rt_i2s_setfreq_before, NULL);

  err |= __rt_cbsys_add(RT_CBSYS_PERIPH_SETFREQ_AFTER, __rt_i2s_setfreq_after, NULL);

  // Configure each channel on a different clock generator to avoid activating a channel
  // when the other is first activated
  for (unsigned int i=0; i<ARCHI_UDMA_NB_I2S; i++)
  {
    __rt_i2s_first[i] = NULL;
    hal_i2s_chmode_set(i, I2S_CHMODE_CH_MODE_CLK(0,0) | I2S_CHMODE_CH_MODE_CLK(1,1));
  }

  if (err) rt_fatal("Unable to initialize i2s driver\n");
}

rt_i2s_t* rt_i2s_open(char *dev_name, rt_i2s_conf_t *conf, rt_event_t*event)
{
  int irq = rt_irq_disable();

  rt_trace(RT_TRACE_DEV_CTRL, "[I2S] Opening i2s device (name: %s)\n", dev_name);

  rt_i2s_dev_t *desc;
  rt_dev_t *dev = NULL;

  if (conf->id != -1)
  {
    desc = &i2s_desc;
  }
  else
  {
    dev = rt_dev_get(dev_name);
    if (dev == NULL) goto error;

    desc = (rt_i2s_dev_t *)dev->desc;
    if (desc == NULL) goto error;
  }

  rt_i2s_t *i2s = desc->open(dev, conf, event);
  if (i2s == NULL) goto error;

  memcpy((void *)&i2s->desc, (void *)desc, sizeof(rt_i2s_dev_t));

  rt_irq_restore(irq);

  return i2s;

error:
  rt_warning("[I2S] Failed to open i2s device\n");
  return NULL;
}

rt_i2s_dev_t i2s_desc = {
  .open      = &__rt_i2s_open,
  .close      = &__rt_i2s_close,
  .capture   = &__rt_i2s_capture,
  .start     = &__rt_i2s_start,
  .stop      = &__rt_i2s_stop,
};
