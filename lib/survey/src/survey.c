/*
 * Copyright 2018, Decawave Limited, All Rights Reserved
 * 
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */


/**
 * @file survey.c
 * @author paul kettle
 * @date 2019
 * 
 * @brief automatic site survey 
 * @details The site survey process involves constructing a matrix of (n * n -1) ranges between n node. 
 * For this, we designate a slot in the superframe that performs a nrng_requst to all other nodes. 
 * We use the ccp->seq number to determine what node make use of this slot.
 *
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <os/os.h>
#include <hal/hal_spi.h>
#include <hal/hal_gpio.h>
#include <stats/stats.h>
#include <bsp/bsp.h>

#include <dw1000/dw1000_dev.h>
#include <dw1000/dw1000_phy.h>
#include <dw1000/dw1000_hal.h>
#if MYNEWT_VAL(SURVEY_ENABLED)
#include <survey/survey.h>
#endif
#if MYNEWT_VAL(TDMA_ENABLED)
#include <tdma/tdma.h>
#endif
#if MYNEWT_VAL(CCP_ENABLED)
#include <ccp/ccp.h>
#endif
#if MYNEWT_VAL(WCS_ENABLED)
#include <wcs/wcs.h>
#endif
#if MYNEWT_VAL(NRNG_ENABLED)
#include <rng/rng.h>
#include <nrng/nrng.h>
#include <rng/slots.h>
#endif
#if MYNEWT_VAL(SURVEY_VERBOSE)
#include <survey/survey_encode.h>
#endif

//#define DIAGMSG(s,u) printf(s,u)
#ifndef DIAGMSG
#define DIAGMSG(s,u)
#endif

static bool rx_complete_cb(struct _dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs);
static bool tx_complete_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs);
static bool rx_timeout_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs);
static bool reset_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs);

STATS_NAME_START(survey_stat_section)
    STATS_NAME(survey_stat_section, request)
    STATS_NAME(survey_stat_section, listen)
    STATS_NAME(survey_stat_section, rx_unsolicited)
    STATS_NAME(survey_stat_section, start_tx_error)
    STATS_NAME(survey_stat_section, start_rx_error)
    STATS_NAME(survey_stat_section, broadcaster)
    STATS_NAME(survey_stat_section, receiver)
    STATS_NAME(survey_stat_section, rx_timeout)
    STATS_NAME(survey_stat_section, reset)
STATS_NAME_END(survey_stat_section)

survey_status_t survey_request(survey_instance_t * survey, uint64_t dx_time);
survey_status_t survey_listen(survey_instance_t * survey, uint64_t dx_time);
survey_status_t survey_broadcaster(survey_instance_t * survey, uint64_t dx_time);
survey_status_t survey_receiver(survey_instance_t * survey, uint64_t dx_time);

/**
 *
 * @return survey_instance_t * 
 */
survey_instance_t * 
survey_init(struct _dw1000_dev_instance_t * inst, uint16_t nnodes){
    assert(inst);
    
    if (inst->survey == NULL ) {
        survey_instance_t * survey = (survey_instance_t *) malloc(sizeof(survey_instance_t) + nnodes * sizeof(survey_ranges_t * )); 
        assert(survey);
        memset(survey, 0, sizeof(survey_instance_t));

        for (uint16_t i = 0; i < nnodes; i++){
            survey->ranges[i] = (survey_ranges_t * ) malloc(sizeof(survey_ranges_t) + nnodes * sizeof(float)); 
            assert(survey->ranges[i]);
            memset(survey->ranges[i], 0, sizeof(survey_ranges_t));
        }

        survey->frame = (survey_broadcast_frame_t *) malloc(sizeof(survey_broadcast_frame_t) + nnodes * sizeof(float)); 
        assert(survey->frame);
        survey_broadcast_frame_t frame = {
            .PANID = 0xDECA,
            .fctrl = FCNTL_IEEE_RANGE_16,
            .dst_address = 0xffff,      //broadcast
            .src_address = inst->my_short_address,
            .code = DWT_SURVEY_BROADCAST
        };
        memcpy(survey->frame, &frame, sizeof(survey_broadcast_frame_t));
        survey->status.selfmalloc = 1;
        survey->nnodes = nnodes; 
        survey->parent = inst;
        inst->survey = survey;
        os_error_t err = os_sem_init(&inst->survey->sem, 0x1); 
        assert(err == OS_OK);

    }else{
        assert(inst->survey->nnodes == nnodes);
    }
    inst->survey->status.initialized = 1;

    inst->survey->config = (survey_config_t){
        .rx_timeout_delay = MYNEWT_VAL(SURVEY_RX_TIMEOUT)
    };

    inst->survey->cbs = (dw1000_mac_interface_t){
        .id = DW1000_SURVEY,
        .rx_complete_cb = rx_complete_cb,
        .tx_complete_cb = tx_complete_cb,
        .rx_timeout_cb = rx_timeout_cb,
        .reset_cb = reset_cb
    };
    dw1000_mac_append_interface(inst, &inst->survey->cbs);

    int rc = stats_init(
                STATS_HDR(inst->survey->stat),
                STATS_SIZE_INIT_PARMS(inst->survey->stat, STATS_SIZE_32),
                STATS_NAME_INIT_PARMS(survey_stat_section)
            );
    assert(rc == 0);

    rc = stats_register("survey", STATS_HDR(inst->survey->stat));
    assert(rc == 0);

    return inst->survey;
}

/** 
 * Deconstructor
 * 
 * @param inst   Pointer to survey_instance_t * 
 * @return void
 */
void 
survey_free(survey_instance_t * inst){
    assert(inst);  
    
    if (inst->status.selfmalloc){
        inst->parent->survey = NULL;
        for (uint16_t i = 0; i < inst->nnodes; i++)
            free(inst->ranges[i]);
        free(inst->frame);
        free(inst);
    }else{
        inst->status.initialized = 0;
    }
}

/**
 * API to initialise the package
 *
 * @return void
 */
void 
survey_pkg_init(void){

    printf("{\"utime\": %lu,\"msg\": \"survey_pkg_init\"}\n",os_cputime_ticks_to_usecs(os_cputime_get32()));

#if MYNEWT_VAL(DW1000_DEVICE_0)
    survey_init(hal_dw1000_inst(0), MYNEWT_VAL(SURVEY_NODES));
#endif
}



void 
survey_slot_range_cb(struct os_event *ev){
    assert(ev);
    assert(ev->ev_arg);

    tdma_slot_t * slot = (tdma_slot_t *) ev->ev_arg;
    tdma_instance_t * tdma = slot->parent;
    dw1000_dev_instance_t * inst = tdma->parent;
    dw1000_ccp_instance_t * ccp = inst->ccp;
    survey_instance_t * survey = inst->survey;
    survey->seq_num = (ccp->idx & ((uint32_t)~0UL << MYNEWT_VAL(SURVEY_MASK))) >> MYNEWT_VAL(SURVEY_MASK);

   
#if MYNEWT_VAL(WCS_ENABLED)
    wcs_instance_t * wcs = ccp->wcs;
    uint64_t dx_time = (ccp->local_epoch + (uint64_t) wcs_dtu_time_adjust(wcs, ((slot->idx * (uint64_t)tdma->period << 16)/tdma->nslots)));
#else
    uint64_t dx_time = (ccp->local_epoch + (uint64_t) (slot->idx * ((uint64_t)tdma->period << 16)/tdma->nslots));
#endif

    if(ccp->idx % survey->nnodes == inst->slot_id){
        dx_time = dx_time & 0xFFFFFFFFFE00UL;
        survey_request(survey, dx_time);
    }
    else{
        dx_time = (dx_time - ((uint64_t)ceilf(dw1000_usecs_to_dwt_usecs(dw1000_phy_SHR_duration(&inst->attrib))) << 16) ) & 0xFFFFFFFE00UL;
        survey_listen(survey, dx_time); 
    }
}


#if MYNEWT_VAL(SURVEY_VERBOSE)
static struct os_callout survey_complete_callout;
static void survey_complete_cb(struct os_event *ev) {
    assert(ev != NULL);
    assert(ev->ev_arg != NULL);

    survey_instance_t * survey = (survey_instance_t *) ev->ev_arg;
    survey_encode(survey, survey->seq_num);
}
#endif


void 
survey_slot_broadcast_cb(struct os_event *ev){
    assert(ev);
    assert(ev->ev_arg);

    tdma_slot_t * slot = (tdma_slot_t *) ev->ev_arg;
    tdma_instance_t * tdma = slot->parent;
    dw1000_dev_instance_t * inst = tdma->parent;
    dw1000_ccp_instance_t * ccp = inst->ccp;
    survey_instance_t * survey = inst->survey;
    survey->seq_num = (ccp->idx & ((uint32_t)~0UL << MYNEWT_VAL(SURVEY_MASK))) >> MYNEWT_VAL(SURVEY_MASK);
    
#if MYNEWT_VAL(WCS_ENABLED)
    wcs_instance_t * wcs = ccp->wcs;
    uint64_t dx_time = (ccp->local_epoch + (uint64_t) wcs_dtu_time_adjust(wcs, ((slot->idx * (uint64_t)tdma->period << 16)/tdma->nslots)));
#else
    uint64_t dx_time = (ccp->local_epoch + (uint64_t) (slot->idx * ((uint64_t)tdma->period << 16)/tdma->nslots));
#endif
    if(ccp->idx % survey->nnodes == inst->slot_id){
        dx_time = dx_time & 0xFFFFFFFFFE00UL;
        survey_broadcaster(survey, dx_time);
    }
    else{
        dx_time = (dx_time - ((uint64_t)ceilf(dw1000_usecs_to_dwt_usecs(dw1000_phy_SHR_duration(&inst->attrib))) << 16) ) & 0xFFFFFFFE00UL;
        survey_receiver(survey, dx_time);  
    }
#if MYNEWT_VAL(SURVEY_VERBOSE)
    if(ccp->idx % survey->nnodes == survey->nnodes - 1 ){
        os_callout_init(&survey_complete_callout, os_eventq_dflt_get(), survey_complete_cb, survey);
        os_eventq_put(os_eventq_dflt_get(), &survey_complete_callout.c_ev);
    }
#endif
}


survey_status_t 
survey_request(survey_instance_t * survey, uint64_t dx_time){
    assert(survey);

    dw1000_dev_instance_t * inst = survey->parent;
    STATS_INC(inst->survey->stat, request);

    uint32_t slot_mask = ~(~0UL << (survey->nnodes));
    dw1000_nrng_request_delay_start(inst, 0xffff, dx_time, DWT_SS_TWR_NRNG, slot_mask, 0);
    survey->ranges[inst->slot_id]->mask = dw1000_nrng_get_ranges(inst, survey->ranges[inst->slot_id]->ranges, survey->nnodes, inst->nrng->idx);
    
    return survey->status;
}


survey_status_t  
survey_listen(survey_instance_t * survey, uint64_t dx_time){
    assert(survey);

    dw1000_dev_instance_t * inst = survey->parent;
    STATS_INC(inst->survey->stat, listen);

    dw1000_set_delay_start(inst, dx_time);
    uint16_t timeout = dw1000_phy_frame_duration(&inst->attrib, sizeof(nrng_request_frame_t))
                        + inst->nrng->config.rx_timeout_delay;         
    dw1000_set_rx_timeout(inst, timeout);
    dw1000_nrng_listen(inst, DWT_BLOCKING);

    return survey->status;
}


survey_status_t  
survey_broadcaster(survey_instance_t * survey, uint64_t dx_time){
    assert(survey);

    os_error_t err = os_sem_pend(&survey->sem,  OS_TIMEOUT_NEVER);
    assert(err == OS_OK);
    STATS_INC(survey->stat, broadcaster);

    dw1000_dev_instance_t * inst = survey->parent;
    survey->frame->mask = survey->ranges[inst->slot_id]->mask;
    survey->frame->seq_num = survey->seq_num;
    survey->frame->slot_id = inst->slot_id;

    uint16_t nnodes = NumberOfBits(survey->frame->mask);
    survey->status.empty = nnodes == 0;
    if (survey->status.empty){
        err = os_sem_release(&survey->sem);
        assert(err == OS_OK);
        return survey->status;
    }

    assert(nnodes < survey->nnodes);
    memcpy(survey->frame->ranges, survey->ranges[inst->slot_id]->ranges, nnodes * sizeof(float));
    uint16_t n = sizeof(struct _survey_broadcast_frame_t) + nnodes * sizeof(float);
    dw1000_write_tx(inst, survey->frame->array, 0, n);
    dw1000_write_tx_fctrl(inst, n, 0, false);
    dw1000_set_delay_start(inst, dx_time); 
    
    survey->status.start_tx_error = dw1000_start_tx(inst).start_tx_error;
    if (survey->status.start_tx_error){
        STATS_INC(survey->stat, start_tx_error);
        if (os_sem_get_count(&survey->sem) == 0) 
            os_sem_release(&survey->sem);
    }else{
        err = os_sem_pend(&survey->sem, OS_TIMEOUT_NEVER); // Wait for completion of transactions 
        assert(err == OS_OK);
        err = os_sem_release(&survey->sem);
        assert(err == OS_OK);
    }
    return survey->status;
}

survey_status_t  
survey_receiver(survey_instance_t * survey, uint64_t dx_time){
    assert(survey);

    dw1000_dev_instance_t * inst = survey->parent;
    os_error_t err = os_sem_pend(&survey->sem,  OS_TIMEOUT_NEVER);
    assert(err == OS_OK);
    STATS_INC(survey->stat, receiver);

    uint16_t n = sizeof(struct _survey_broadcast_frame_t) + survey->nnodes * sizeof(float);
    uint16_t timeout = dw1000_phy_frame_duration(&inst->attrib, n) 
                        + survey->config.rx_timeout_delay;
    dw1000_set_rx_timeout(inst, timeout); 

    survey->status.start_rx_error = dw1000_start_rx(inst).start_rx_error;
    if (survey->status.start_rx_error){
        STATS_INC(survey->stat, start_rx_error);
        err = os_sem_release(&survey->sem);
        assert(err == OS_OK); 
    }else {
        err = os_sem_pend(&survey->sem, OS_TIMEOUT_NEVER); // Wait for completion of transactions 
        assert(err == OS_OK);
        err = os_sem_release(&survey->sem);
        assert(err == OS_OK);
    }
    return survey->status;
}

/**
 * API for receive survey broadcasts information.
 * @param inst Pointer to dw1000_dev_instance_t.
 * @return true on sucess
 */
static bool 
rx_complete_cb(struct _dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs)
{   
    assert(inst->survey);
    survey_instance_t * survey = inst->survey; 

    if (inst->fctrl != FCNTL_IEEE_RANGE_16)
        return false;

    if(os_sem_get_count(&survey->sem) == 1){ 
        // unsolicited inbound
        STATS_INC(survey->stat, rx_unsolicited);
        return false;
    }
    
    if (inst->frame_len < sizeof(survey_broadcast_frame_t))
       return false;
  
    survey_broadcast_frame_t * frame = ((survey_broadcast_frame_t * ) inst->rxbuf);
    printf("rx_complete_cb\n");

    if (frame->dst_address != 0xffff)
        return false;

    switch(frame->code) {
        case DWT_SURVEY_BROADCAST:
            {   
                if (frame->cell_id != inst->cell_id)
                    return false;
                if (frame->seq_num != survey->seq_num)
                    return false;
                uint16_t n = sizeof(survey_broadcast_frame_t) + survey->nnodes * sizeof(float);
                if (inst->frame_len <= n && frame->slot_id < survey->nnodes) {
                    memcpy(survey->ranges[frame->slot_id], inst->rxbuf, inst->frame_len);
                }
            }
            break;
        default: 
            return false;
    }

    os_error_t err = os_sem_release(&survey->sem);
    assert(err == OS_OK);

    return true;
}


/**
 * API for transmission complete callback.
 *
 * @param inst  Pointer to dw1000_dev_instance_t.
 *
 * @return true on sucess
 */
static bool
tx_complete_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs){

    if(os_sem_get_count(&inst->survey->sem) == 1){ 
        // unsolicited inbound
        return false;
    }
    os_error_t err = os_sem_release(&inst->survey->sem);
    assert(err == OS_OK);
    
    return false;
}


/**
 * API for transmission complete callback.
 *
 * @param inst  Pointer to dw1000_dev_instance_t.
 *
 * @return true on sucess
 */
static bool
rx_timeout_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs){
 
    if(os_sem_get_count(&inst->survey->sem) == 0){ 
        os_error_t err = os_sem_release(&inst->survey->sem);  
        assert(err == OS_OK);
        STATS_INC(inst->survey->stat, rx_timeout);
        return true;
    }
    else
        return false;
}


/** 
 * API for reset_cb of survey interface
 *
 * @param inst   Pointer to dw1000_dev_instance_t. 
 * @return true on sucess
 */
static bool
reset_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs){

   if(os_sem_get_count(&inst->survey->sem) == 0){ 
        os_error_t err = os_sem_release(&inst->survey->sem);  
        assert(err == OS_OK);
        STATS_INC(inst->survey->stat, reset);
        return false;
    }
    else 
        return false;

}

