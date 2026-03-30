#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "pcanpro_can.h"
#include "pcan_usbpro_fw.h"
#include "pcanfd_usb_fw.h"
#include "pcanpro_timestamp.h"
#include "pcanpro_protocol.h"
#include "pcanpro_led.h"
#include "pcanpro_usbd.h"
#include "usb_device.h"
#include "utils.h"
#include "debug.h"
#include "can.h"
#include "fw_diag.h"

#define CAN_CHANNEL_MAX     (1)

struct pcan_usbfd_fw_info
{
  uint16_t  size_of;        /* sizeof this */
  uint16_t  type;           /* type of this structure */
  uint8_t   hw_type;        /* Type of hardware (HW_TYPE_xxx) */
  uint8_t   bl_version[3];  /* Bootloader version */
  uint8_t   hw_version;     /* Hardware version (PCB) */
  uint8_t   fw_version[3];  /* Firmware version */
  uint32_t  dev_id[2];      /* "device id" per CAN */
  uint32_t  ser_no;          /* S/N */
  uint32_t  flags;          /* special functions / capabilities */
  uint8_t   unk[8];         /* pipe numbers with offsets */
  uint8_t   can_bus_cnt;
  uint8_t   instance_idx;
  uint8_t   dummy1;
  uint8_t   dummy2;
  uint8_t   guid[16];
  uint8_t   fw_version_addon[28];
} __attribute__ ((packed));

static struct
{
  uint32_t device_nr;
  uint32_t last_bus_load_update;
  uint32_t last_time_sync;
  uint32_t last_time_flush;
  uint8_t  can_drv_loaded;
  uint8_t  lin_drv_loaded;

  struct
  {
    /* config */
    uint8_t   silient;
    uint8_t   bus_active;
    uint8_t   loopback;
    uint8_t   err_mask;
    uint32_t  device_id;
    uint32_t  channel_nr;
    uint16_t  opt_mask;
    uint16_t  usb_opt_mask;

    uint8_t   led_is_busy;

    /* slow speed */
    struct ucan_timing_slow slow_br;
    /* can fd , data fast speed */
    struct ucan_timing_fast fast_br;

    /* clock */
    uint32_t can_clock;

    uint32_t nominal_qt;
    uint32_t data_qt;
    uint32_t tx_time_ns;
    uint32_t rx_time_ns;

    uint32_t bus_load;
  }
  can[CAN_CHANNEL_MAX];
}
pcan_device =
{
  .device_nr = 0,

  .can[0] = 
  {
    .device_id = 0xFFFFFFFF,
    .channel_nr = 0xFFFFFFFF,
    .can_clock = 80000000u
  },
  /* only 1 CAN channel - CAN_BUS_TOTAL=1 */
};

static uint32_t pcan_map_uid_to_device_id( uint32_t uid_low )
{
  switch( uid_low )
  {
    case 0x00480028UL:
      return 0x001111FFUL;
    case 0x0047001EUL:
      return 0x002222FFUL;
    case 0x00480017UL:
      return 0x003333FFUL;
    case 0x00490052UL:
      return 0x004444FFUL;
    default:
      return uid_low;
  }
}

#define PCAN_USB_DATA_BUFFER_SIZE   1024
#define PCAN_USB_CMD_BUFFER_SIZE    512
static uint8_t resp_buffer[2][PCAN_USB_DATA_BUFFER_SIZE];
static uint8_t drv_load_packet[16];
static uint8_t cmd_buffer[PCAN_USB_CMD_BUFFER_SIZE];
static uint16_t cmd_pos = 0;

static uint16_t data_pos = 0;
static uint8_t   data_buffer[PCAN_USB_DATA_BUFFER_SIZE];

void *pcan_data_alloc_buffer( uint16_t type, uint16_t size )
{
  uint16_t aligned_size = (size+(4-1))&(~(4-1)); //向上取整到4对齐
  if( sizeof( data_buffer ) < (aligned_size+data_pos+4) )
    return (void*)0;
  struct ucan_msg *pmsg = (void*)&data_buffer[data_pos];

  pmsg->size = aligned_size;
  pmsg->type = type;
  pmsg->ts_low = pcan_timestamp_us();
  pmsg->ts_high = 0;

  data_pos += aligned_size;
  return pmsg;
}

static struct t_m2h_fsm resp_fsm[2] = 
{
  [0] = {
    .state = 0,
    .ep_addr = PCAN_USB_EP_CMDIN,
    .pdbuf = resp_buffer[0],
    .dbsize = PCAN_USB_DATA_BUFFER_SIZE,
  },
  [1] = {
    .state = 0,
    .ep_addr = PCAN_USB_EP_MSGIN_CH1,
    .pdbuf = resp_buffer[1],
    .dbsize = PCAN_USB_DATA_BUFFER_SIZE,
  }
};

/* low level requests */
uint8_t pcan_protocol_device_setup( USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req )
{
  PRINT_DEBUG("USB setup: bReq=0x%02x wVal=0x%04x wLen=%u", req->bRequest, req->wValue, req->wLength);
  g_fw_diag.last_setup_request = req->bRequest;
  g_fw_diag.last_setup_wvalue = req->wValue;
  g_fw_diag.last_setup_wlength = req->wLength;
  if( (req->bRequest == USB_VENDOR_REQUEST_INFO) &&
      (req->wValue == USB_VENDOR_REQUEST_wVALUE_INFO_FIRMWARE) )
  {
    g_fw_diag.fw_info_req_count++;
    g_fw_diag.fw_info_req_wlength = req->wLength;
  }
  switch( req->bRequest )
  {
    case USB_VENDOR_REQUEST_INFO:
      switch( req->wValue )
      {
        case USB_VENDOR_REQUEST_wVALUE_INFO_FIRMWARE:
        {
          static struct pcan_usbfd_fw_info fwi =
          {
            .size_of = sizeof( struct pcan_usbfd_fw_info ),
            .type = 2,
            .hw_type = 1,
#if ( PCAN_X6 == 1 )
            .bl_version = { 1, 1, 0 },
            .hw_version = 3,
#else
            .bl_version = { 2, 1, 0 }, /* bootloader v > 2 support massstorage mode */
            .hw_version = 2,
#endif
            .fw_version = { 3, 2, 0 },
            .dev_id[0] = 0xFFFFFFFF,
            .dev_id[1] = 0xFFFFFFFF,
            .ser_no = 0xFFFFFFFF,
            .flags = 0x00000000,
            .unk = {
              PCAN_USB_EP_CMDOUT,
              PCAN_USB_EP_CMDIN,
              PCAN_USB_EP_MSGOUT_CH1,
              0x00,
              PCAN_USB_EP_MSGIN_CH1,
              0x00,
              0x00,
              0x00
            },
            .can_bus_cnt = CAN_CHANNEL_MAX,
            .instance_idx = 0,
            .dummy1 = 0,
            .dummy2 = 0,
            .guid = { 0 },
            .fw_version_addon = { 0 },
          };
          /* windows/linux has different struct size */
          uint16_t send_len = req->wLength;
          if( send_len > sizeof( fwi ) )
            send_len = sizeof( fwi );
          fwi.size_of = sizeof( fwi );
          fwi.dev_id[0] = pcan_device.can[0].device_id;
          fwi.dev_id[1] = 0xFFFFFFFF;  /* single channel */
          fwi.ser_no = pcan_device.device_nr;
          memcpy( &fwi.guid[0], (void *)(0x1FFF7590UL), 12 );
          return USBD_CtlSendData( pdev,  (void*)&fwi, send_len );
        }
        default:
          assert(0);
        break;
      }
      break;
    case USB_VENDOR_REQUEST_FKT:
      switch( req->wValue )
      {
        case USB_VENDOR_REQUEST_wVALUE_SETFKT_BOOT:
          break;
        case USB_VENDOR_REQUEST_wVALUE_SETFKT_INTERFACE_DRIVER_LOADED:
        {
          USBD_CtlPrepareRx( pdev, drv_load_packet, 16 );
          return USBD_OK;
        }
        break;
        default:
          assert(0);
          break;
      }
      break;
    case USB_VENDOR_REQUEST_ZERO:
      break;
    default:
      USBD_CtlError( pdev, req );
      return USBD_FAIL; 
  }

  return USBD_FAIL;
}

void pcan_ep0_receive( void )
{
  PRINT_DEBUG("EP0 rx: drv_load[0]=%u drv_load[1]=%u", drv_load_packet[0], drv_load_packet[1]);
  if( drv_load_packet[0] == 0 )
  {
    pcan_flush_ep( PCAN_USB_EP_MSGIN_CH1 );
    pcan_flush_ep( PCAN_USB_EP_CMDIN );
    pcan_device.can_drv_loaded = drv_load_packet[1];
    pcan_device.last_time_sync = 0;
    cmd_pos = 0;
    PRINT_DEBUG("CAN driver loaded=%u", pcan_device.can_drv_loaded);
    pcan_led_set_mode( LED_STAT, LED_MODE_BLINK_SLOW, 0xFFFFFFFF );
  }
  else
    pcan_device.lin_drv_loaded = drv_load_packet[1];
}

int pcan_protocol_rx_frame( uint8_t channel, struct t_can_msg *pmsg )
{
  struct ucan_rx_msg *pcan_msg = pcan_data_alloc_buffer( UCAN_MSG_CAN_RX, sizeof(struct ucan_rx_msg) + pmsg->size );
  if( !pcan_msg )
    return -1;

  if( !(pmsg->flags & MSG_FLAG_ECHO) )
  {
    uint32_t nqt = pcan_device.can[channel].nominal_qt;
    uint32_t dqt = pcan_device.can[channel].data_qt;
    pcan_device.can[channel].rx_time_ns += pcan_can_msg_time( pmsg, nqt, dqt );
    if( !pcan_device.can[channel].led_is_busy )
    {
      pcan_led_set_mode( channel ? LED_CH1_RX:LED_CH0_RX, LED_MODE_BLINK_FAST, 237 );
    }
  }

  if( pmsg->size > CAN_PAYLOAD_MAX_SIZE )
    pmsg->size = CAN_PAYLOAD_MAX_SIZE;
  pcan_msg->channel_dlc = UCAN_MSG_CHANNEL_DLC( channel, utils_byte_count_to_dlc(pmsg->size) );
  
  pcan_msg->client = pmsg->dummy;
  pcan_msg->flags = 0;
  pcan_msg->tag_low = 0;
  pcan_msg->tag_high = 0;

  /* we support only regular frames */
  if( pmsg->flags & MSG_FLAG_RTR )
    pcan_msg->flags |= UCAN_MSG_RTR;
  if( pmsg->flags & MSG_FLAG_EXT )
    pcan_msg->flags |= UCAN_MSG_EXT_ID;
  if( pmsg->flags & MSG_FLAG_FD )
  {
    pcan_msg->flags |= UCAN_MSG_EXT_DATA_LEN;
    if( pmsg->flags & MSG_FLAG_BRS )
      pcan_msg->flags |= UCAN_MSG_BITRATE_SWITCH;
    if( pmsg->flags & MSG_FLAG_ESI )
      pcan_msg->flags |= UCAN_MSG_ERROR_STATE_IND;
  }
  if( pmsg->flags & MSG_FLAG_ECHO )
  {
    pcan_msg->flags |= UCAN_MSG_API_SRR | UCAN_MSG_HW_SRR;
    pcan_msg->ts_low = pcan_timestamp_us();
  }
  else
  {
    pcan_msg->ts_low = pmsg->timestamp;
  }

  pcan_msg->can_id = pmsg->id;
  memcpy( pcan_msg->d, pmsg->data, pmsg->size );
	
  return 0;
}

int pcan_protocol_tx_frame_cb( uint8_t channel, struct t_can_msg *pmsg )
{
  if( pmsg->flags & MSG_FLAG_ECHO )
  {
    (void)pcan_protocol_rx_frame( channel, pmsg );
  }

  if( !pcan_device.can[channel].led_is_busy )
  {
    pcan_led_set_mode( channel ? LED_CH1_TX:LED_CH0_TX, LED_MODE_BLINK_FAST, 237 );
  }

  uint32_t nqt = pcan_device.can[channel].nominal_qt;
  uint32_t dqt = pcan_device.can[channel].data_qt;
  pcan_device.can[channel].tx_time_ns += pcan_can_msg_time( pmsg, nqt, dqt );
  return 0;
}

int pcan_protocol_tx_frame( struct ucan_tx_msg *pmsg )
{

  struct t_can_msg msg = { 0 };
  uint8_t channel;

  g_fw_diag.usb_tx_msgs++;
  channel = UCAN_MSG_CHANNEL(pmsg);

  if( channel >= CAN_CHANNEL_MAX )
    return -1;

  msg.id = pmsg->can_id;

  /* CAN-FD frame */
  if( pmsg->flags & UCAN_MSG_EXT_DATA_LEN )
  {
    msg.flags |= MSG_FLAG_FD;
    if( pmsg->flags & UCAN_MSG_BITRATE_SWITCH )
      msg.flags |= MSG_FLAG_BRS;
    if( pmsg->flags & UCAN_MSG_ERROR_STATE_IND )
      msg.flags |= MSG_FLAG_ESI;
    
	msg.size = utils_dlc_to_byte_count(UCAN_MSG_DLC(pmsg));
  }
  else
  {
    msg.size = UCAN_MSG_DLC(pmsg);
  }

  if( msg.size > sizeof( msg.data ) )
    return -1;

  /* TODO: process UCAN_MSG_SINGLE_SHOT, UCAN_MSG_HW_SRR, UCAN_MSG_ERROR_STATE_IND */
  if( pmsg->flags & UCAN_MSG_RTR )
    msg.flags |= MSG_FLAG_RTR;
  if( pmsg->flags & UCAN_MSG_EXT_ID )
    msg.flags |= MSG_FLAG_EXT;
  if( pmsg->flags & (/*UCAN_MSG_API_SRR|*/UCAN_MSG_HW_SRR) )
  {
    msg.flags |= MSG_FLAG_ECHO;
    msg.dummy = pmsg->client;
  }

  memcpy( msg.data, pmsg->d, msg.size );

  msg.timestamp = pcan_timestamp_us();

  if( pcan_can_write( channel, &msg ) < 0 )
  {
    /* TODO: tx queue overflow ? */
    ;
  }
  return 0;
}

static int pcan_protocol_send_status( uint8_t channel, uint8_t status )
{
  struct ucan_status_msg *ps = pcan_data_alloc_buffer( UCAN_MSG_STATUS, sizeof( struct ucan_status_msg ) );
  if( !ps )
    return -1;

  g_fw_diag.status_msgs_built++;
  ps->channel_p_w_b = channel&0x0f;
  ps->channel_p_w_b |= (status&0x0f)<<4;
  return 0;
}

//extern uint32_t system_get_can_clock(void);
extern int pcan_can_set_canfdclock(uint32_t can_clock);
int pcan_protocol_set_baudrate( uint8_t channel, struct t_can_bitrate *pbitrate, struct t_can_bitrate *pdata_bitrate )
{
  uint32_t   bitrate, sample; //, pcan_bitrate, pcan_brp;
  struct t_can_bitrate *pcur;

  if( pbitrate )
    pcur = pbitrate;
  else if( pdata_bitrate )
    pcur = pdata_bitrate;
  else
    return -1;
  
  //pcan_brp = ((system_get_can_clock()/1000000u)*pcur->brp)/(pcan_device.can[channel].can_clock/1000000u);
  //pcan_bitrate = (((system_get_can_clock())/pcan_brp)/(1/*tq*/ + pcur->tseg1 + pcur->tseg2 ));
  bitrate = (((pcan_device.can[channel].can_clock)/pcur->brp)/(1/*tq*/ + pcur->tseg1 + pcur->tseg2 ));
  sample = 1000 * (1 + pcur->tseg1) / (1 + pcur->tseg1 + pcur->tseg2);
  
  PRINT_DEBUG("%s bitrate=%lu, sample=%lu, brp=%u, tseg1=%u, tseg2=%u, sjw=%u", 
  										 (pdata_bitrate == NULL)? "nom":"data", \
	  										bitrate, sample, \
	  										pcur->brp, pcur->tseg1, \
	  										pcur->tseg2, pcur->sjw);
  //pcan_can_set_bitrate( channel, bitrate, ( pcur == pdata_bitrate ) );
  pcan_can_set_bitrate_ex( channel, pcur->brp, pcur->tseg1, pcur->tseg2, pcur->sjw, ( pcur == pdata_bitrate ) );
  
  if( pcur == pdata_bitrate )
  {
    pcan_device.can[channel].data_qt = 1000000000u/bitrate;
  }
  else
  {
    pcan_device.can[channel].nominal_qt = 1000000000u/bitrate;
  }
  
  return 0;
}

static void pcan_protocol_process_cmd( uint8_t *ptr, uint16_t size )
{
  struct ucan_command *pcmd = (void*)ptr;
  uint8_t bus_cfg_seen[CAN_CHANNEL_MAX] = { 0 };
  uint8_t bus_mode_seen[CAN_CHANNEL_MAX] = { 0 };

  while( size >= sizeof( struct ucan_command ) )
  {
    g_fw_diag.cmd_packets++;
    switch( UCAN_CMD_OPCODE( pcmd ) )
    {
      case UCAN_CMD_NOP:
        break;
      case UCAN_CMD_RESET_MODE:
      {
        g_fw_diag.cmd_reset_mode++;
      	PRINT_DEBUG("UCAN_CMD_RESET_MODE");
        if( UCAN_CMD_CHANNEL(pcmd) < CAN_CHANNEL_MAX )
        {
          pcan_can_set_bus_active( UCAN_CMD_CHANNEL(pcmd) , 0 );
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].bus_active = 0;

          /* update ISO mode only on inactive bus */
          uint8_t bus_iso_mode = ( pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].opt_mask & UCAN_OPTION_ISO_MODE ) != 0;
          pcan_can_set_iso_mode( UCAN_CMD_CHANNEL(pcmd), bus_iso_mode );
        }
      }
        break;
      case UCAN_CMD_NORMAL_MODE:
      {
        g_fw_diag.cmd_normal_mode++;
      	PRINT_DEBUG("UCAN_CMD_NORMAL_MODE");
        if( UCAN_CMD_CHANNEL(pcmd) < CAN_CHANNEL_MAX )
        {
          bus_mode_seen[UCAN_CMD_CHANNEL(pcmd)] = 1;
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].channel_nr = 0x2001;
          if( pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].silient  )
          {
            pcan_can_set_silent( UCAN_CMD_CHANNEL(pcmd) , 0 );
            pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].silient = 0;
          }
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].bus_active = 1;
          pcan_can_set_bus_active( UCAN_CMD_CHANNEL(pcmd) , 1 );
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].channel_nr = can_is_opened() ? 0x2002 : 0xE002;
          pcan_protocol_send_status( UCAN_CMD_CHANNEL(pcmd), 0 );
        }
      }
        break;
      case UCAN_CMD_LISTEN_ONLY_MODE:
      {
        g_fw_diag.cmd_listen_only_mode++;
      	PRINT_DEBUG("UCAN_CMD_LISTEN_ONLY_MODE");
        if( UCAN_CMD_CHANNEL(pcmd) < CAN_CHANNEL_MAX )
        {
          bus_mode_seen[UCAN_CMD_CHANNEL(pcmd)] = 1;
          pcan_can_set_silent( UCAN_CMD_CHANNEL(pcmd) , 1 );
          pcan_can_set_bus_active( UCAN_CMD_CHANNEL(pcmd) , 1 );
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].silient = 1;
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].bus_active = 1;
          pcan_protocol_send_status( UCAN_CMD_CHANNEL(pcmd), 0 );
        }
      }
        break;
      case UCAN_CMD_TIMING_SLOW:
        g_fw_diag.cmd_timing_slow++;
	  	PRINT_DEBUG("UCAN_CMD_TIMING_SLOW");
        if( UCAN_CMD_CHANNEL(pcmd) < CAN_CHANNEL_MAX )
        {
          bus_cfg_seen[UCAN_CMD_CHANNEL(pcmd)] = 1;
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].channel_nr = 0x1001;
          struct t_can_bitrate br;
          struct ucan_timing_slow *ptiming = (void*)pcmd;
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].slow_br = *ptiming;
          
          br.brp = ptiming->brp + 1;
          br.tseg1 = ptiming->tseg1 + 1;
          br.tseg2 = ptiming->tseg2 + 1;
          br.sjw = (ptiming->sjw_t /* & 0x0f */)+1;
          (void)((ptiming->sjw_t&0x80) != 0);

          pcan_protocol_set_baudrate( UCAN_CMD_CHANNEL(pcmd), &br, 0 );
        }
        break;
      /* only for CAN-FD */
      case UCAN_CMD_TIMING_FAST:
        g_fw_diag.cmd_timing_fast++;
	  	PRINT_DEBUG("UCAN_CMD_TIMING_FAST");
        if( UCAN_CMD_CHANNEL(pcmd) < CAN_CHANNEL_MAX )
        {
          bus_cfg_seen[UCAN_CMD_CHANNEL(pcmd)] = 1;
          struct t_can_bitrate br;
          struct ucan_timing_fast *ptiming = (void*)pcmd;
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].fast_br = *ptiming;

          br.brp = ptiming->brp + 1;
          br.tseg1 = ptiming->tseg1 + 1;
          br.tseg2 = ptiming->tseg2 + 1;
          br.sjw = (ptiming->sjw & 0x0f)+1;
          
          pcan_protocol_set_baudrate( UCAN_CMD_CHANNEL(pcmd), 0, &br );
        }
        break;
      case UCAN_CMD_SET_STD_FILTER:
	  	PRINT_DEBUG("UCAN_CMD_SET_STD_FILTER");
        if( UCAN_CMD_CHANNEL(pcmd) >= CAN_CHANNEL_MAX )
          break;
        pcan_can_set_filter_mask( UCAN_CMD_CHANNEL(pcmd), 0, 0, 0, 0 );
        break;
      case UCAN_CMD_RESERVED2:
	  	PRINT_DEBUG("UCAN_CMD_RESERVED2");
        break;
      case UCAN_CMD_FILTER_STD:
	  	PRINT_DEBUG("UCAN_CMD_FILTER_STD");
        if( UCAN_CMD_CHANNEL(pcmd) >= CAN_CHANNEL_MAX )
          break;
        pcan_can_set_filter_mask( UCAN_CMD_CHANNEL(pcmd), 0, 0, 0, 0 );
        break;
      case UCAN_CMD_TX_ABORT:
	  	PRINT_DEBUG("UCAN_CMD_TX_ABORT");
        break;
      case UCAN_CMD_WR_ERR_CNT:
	  	PRINT_DEBUG("UCAN_CMD_WR_ERR_CNT");
        break;
      case UCAN_CMD_SET_EN_OPTION:
        g_fw_diag.cmd_set_option++;
        if( UCAN_CMD_CHANNEL(pcmd) < CAN_CHANNEL_MAX )
        {
          struct ucan_usb_option *popt = (void*)pcmd;
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].opt_mask |= popt->ext_mask;
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].usb_opt_mask |= popt->usb_mask;

		  PRINT_DEBUG("UCAN_CMD_SET_EN_OPTION: ucan=0x%04x usb=0x%04x", popt->ext_mask, popt->usb_mask);
          if ((popt->ext_mask & UCAN_OPTION_ISO_MODE)  != 0)
          {					
              uint8_t bus_iso_mode = 0;
              pcan_can_set_iso_mode( UCAN_CMD_CHANNEL(pcmd), bus_iso_mode );
          }
        }
        break;
      case UCAN_CMD_CLR_DIS_OPTION:
        g_fw_diag.cmd_clr_option++;
        if( UCAN_CMD_CHANNEL(pcmd) < CAN_CHANNEL_MAX )
        {
          struct ucan_usb_option *popt = (void*)pcmd;
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].opt_mask &= (uint16_t)~popt->ext_mask;
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].usb_opt_mask &= (uint16_t)~popt->usb_mask;

		  PRINT_DEBUG("UCAN_CMD_CLR_DIS_OPTION: ucan=0x%04x usb=0x%04x", popt->ext_mask, popt->usb_mask);
		  if ((popt->ext_mask & UCAN_OPTION_ISO_MODE)  != 0)
          {					
              uint8_t bus_iso_mode = 1;
              pcan_can_set_iso_mode( UCAN_CMD_CHANNEL(pcmd), bus_iso_mode );
          }
        }
        break;
      case UCAN_CMD_SET_ERR_GEN1:
	  	PRINT_DEBUG("UCAN_CMD_SET_ERR_GEN1");
        break;
      case UCAN_CMD_SET_ERR_GEN2:
	  	PRINT_DEBUG("UCAN_CMD_SET_ERR_GEN2");
        break;
      case UCAN_CMD_DIS_ERR_GEN:
	  	PRINT_DEBUG("UCAN_CMD_DIS_ERR_GEN");
        break;
      case UCAN_CMD_RX_BARRIER:
	  	PRINT_DEBUG("UCAN_CMD_RX_BARRIER");
        break;
      case UCAN_CMD_SET_ERR_GEN_S:
	  	PRINT_DEBUG("UCAN_CMD_SET_ERR_GEN_S");
        break;
      case UCAN_USB_CMD_CLK_SET:
        g_fw_diag.cmd_clk_set++;
	  	PRINT_DEBUG("UCAN_USB_CMD_CLK_SET");
        if( UCAN_CMD_CHANNEL(pcmd) < CAN_CHANNEL_MAX )
        {
          bus_cfg_seen[UCAN_CMD_CHANNEL(pcmd)] = 1;
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].channel_nr = 0x3001;
          struct ucan_usb_clock *pclock = (void*)pcmd;
          switch( pclock->mode )
          {
            default:
            case UCAN_USB_CLK_80MHZ:
              pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].can_clock = 80000000u;
            break;
            case UCAN_USB_CLK_60MHZ:
              pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].can_clock = 60000000u;
            break;
            case UCAN_USB_CLK_40MHZ:
              pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].can_clock = 40000000u;
            break;
            case UCAN_USB_CLK_30MHZ:
              pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].can_clock = 30000000u;
            break;
            case UCAN_USB_CLK_24MHZ:
              pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].can_clock = 24000000u;
            break;
            case UCAN_USB_CLK_20MHZ:
              pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].can_clock = 20000000u;
            break;
          }

		  //设置CANFD时钟频率
          pcan_can_set_canfdclock( pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].can_clock );
        }
        break;
      case UCAN_USB_CMD_LED_SET:
        g_fw_diag.cmd_led_set++;
	  	PRINT_DEBUG("UCAN_USB_CMD_LED_SET");
        if( UCAN_CMD_CHANNEL(pcmd) < CAN_CHANNEL_MAX )
        {
          struct ucan_usb_led *pled = (void*)pcmd;
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].led_is_busy = pled->mode;
          pcan_led_set_mode( UCAN_CMD_CHANNEL(pcmd), pled->mode, 0xFFFFFFFF );
        }
        break;
      case UCAN_USB_CMD_DEVID_SET:
      {
        g_fw_diag.cmd_devid_set++;
      	PRINT_DEBUG("UCAN_USB_CMD_DEVID_SET");
        struct ucan_usb_device_id *pdevid = (void*)pcmd;
        if( UCAN_CMD_CHANNEL(pcmd) < CAN_CHANNEL_MAX )
        {
          pcan_device.can[UCAN_CMD_CHANNEL(pcmd)].device_id = pdevid->device_id;
        }
        break;
      }
      case 0x87: /* CAN FD ISO MODE, 0xff - enable, 0x55 - disable  */
      {
        /* do nothing here */
	  	PRINT_DEBUG("Entry cmd 0x87");
        break;
      }
      case UCAN_CMD_END_OF_COLLECTION:
        goto finalize;
      default:
        g_fw_diag.last_tx_id = pcmd->opcode_channel;
        g_fw_diag.last_tx_size_flags++;
        break;
    }

    size -= sizeof( struct ucan_command );
    ++pcmd;
  }

finalize:
  for( int i = 0; i < CAN_CHANNEL_MAX; i++ )
  {
    if( !bus_cfg_seen[i] || bus_mode_seen[i] || pcan_device.can[i].bus_active )
      continue;
    pcan_can_set_bus_active( i, 1 );
    pcan_device.can[i].bus_active = can_is_opened() ? 1 : 0;
    pcan_protocol_send_status( i, 0 );
  }
}

static uint8_t pcan_protocol_cmd_has_eoc( uint8_t *ptr, uint16_t size )
{
  while( size >= sizeof( struct ucan_command ) )
  {
    struct ucan_command *pcmd = (void*)ptr;
    if( UCAN_CMD_OPCODE( pcmd ) == UCAN_CMD_END_OF_COLLECTION )
      return 1;
    ptr += sizeof( struct ucan_command );
    size -= sizeof( struct ucan_command );
  }
  return 0;
}

void pcan_protocol_process_data( uint8_t ep, uint8_t *ptr, uint16_t size )
{
  /* message data ? */
  struct ucan_msg *pmsg = 0;
  
  if( ep == PCAN_USB_EP_CMDOUT )
  {
    if( size > sizeof( cmd_buffer ) )
    {
      cmd_pos = 0;
      return;
    }

    if( (uint32_t)cmd_pos + size > sizeof( cmd_buffer ) )
      cmd_pos = 0;

    memcpy( &cmd_buffer[cmd_pos], ptr, size );
    cmd_pos += size;

    if( !pcan_protocol_cmd_has_eoc( cmd_buffer, cmd_pos ) && ( size == 64 ) )
      return;

    pcan_protocol_process_cmd( cmd_buffer, cmd_pos );
    cmd_pos = 0;
    return;
  }
  
  while( size )
  {
    if( size < 4 )
      break;
    pmsg = (void*)ptr;
    g_fw_diag.usb_out_last_type = pmsg->type;
    g_fw_diag.usb_out_last_msg_size = pmsg->size;
    if( !pmsg->size || !pmsg->type )
      break;
    if( size < pmsg->size )
    {
#if 0
    	struct ucan_tx_msg *p_tx_msg =  (struct ucan_tx_msg *)pmsg;
    	PRINT_TRACE("can_id=0x%x,size=%u,type=0x%x,channel_dlc=0x%x,flags=0x%x",
			p_tx_msg->can_id,
			p_tx_msg->size,
	    	p_tx_msg->type,
	    	p_tx_msg->channel_dlc,
	    	p_tx_msg->flags
	    	);
		PRINT_TRACE_BUFFER(p_tx_msg->d, p_tx_msg->size);
#endif
		//bug??
		if( (size == 64) && ((pmsg->size == 68) || (pmsg->size == 84)) )
		{
			pmsg->size = (pmsg->size - 20);
			size = pmsg->size;
		}
		else
		{
			break;
		}
    }
    
    size -= pmsg->size;
    ptr += pmsg->size;

    switch( pmsg->type )
    {
      //to host only
      //UCAN_MSG_ERROR:
      //UCAN_MSG_BUSLOAD:
      case UCAN_MSG_CAN_TX:
        pcan_protocol_tx_frame( (struct ucan_tx_msg *)pmsg );
        break;
      case UCAN_MSG_CAN_TX_PAUSE:
        /* TODO: */
        break;
      case UCAN_CMD_END_OF_COLLECTION:
      case 0xffff:
        return;
      default:
        assert( 0 );
        break;  
    }
  }
}

void pcan_protocol_init( void )
{
  pcan_device.device_nr = *(uint32_t *)(0x1FFF7590UL);
  pcan_device.can[0].device_id = pcan_map_uid_to_device_id( pcan_device.device_nr );
  pcan_device.can[0].channel_nr = pcan_device.can[0].device_id;

  PRINT_DEBUG("pcan_protocol_init: SN=0x%08lx CAN_CH_MAX=%d", pcan_device.device_nr, CAN_CHANNEL_MAX);

  pcan_can_init_ex( CAN_BUS_1, 500000 );

  pcan_can_set_filter_mask( CAN_BUS_1, 0, MSG_FLAG_STD, 0, 0 );
  pcan_can_set_filter_mask( CAN_BUS_1, 0, MSG_FLAG_EXT, 0, 0 );
  
  pcan_can_set_iso_mode( CAN_BUS_1, 0 );

  pcan_can_install_rx_callback( CAN_BUS_1, pcan_protocol_rx_frame );
  pcan_can_install_tx_callback( CAN_BUS_1, pcan_protocol_tx_frame_cb );
}

void pcan_protocol_poll( void )
{
  enum { PCAN_USB_CALIBRATION_INTERVAL_MS = 50 };
  uint32_t ts_ms = pcan_timestamp_millis();
  uint32_t ts_us = pcan_timestamp_us();

  //pcan_can_poll();

  /* flush data */
  if( data_pos > 0 )
  {
    /* endmark */
    *(uint32_t*)&data_buffer[data_pos] = 0x00000000;
    uint16_t flush_size = data_pos + 4;
    /* align to 64 */
    flush_size += (64-1);
    flush_size &= ~(64-1);
    int res = pcan_flush_data( &resp_fsm[1], data_buffer, flush_size );
    if( res )
    { 
      data_pos = 0;
      pcan_device.last_time_flush = ts_us;
    }
  }
#if 0
  else
  {
    ts_us -= pcan_device.last_time_flush;
    if( pcan_device.last_time_flush && ( ts_us > 800 ) )
    {
      int res = pcan_flush_data( &resp_fsm[1], 0, 0 );
      if( res )
      {
        pcan_device.last_time_flush = 0;
      }
    }
  }
  #endif

  /* timesync part */
  if( !pcan_device.can_drv_loaded )
    return;

  /* update bus load each 250ms */
  if( ( ts_ms - pcan_device.last_bus_load_update ) >= 250u )
  {
    pcan_device.last_bus_load_update = ts_ms;
    for( int i = 0; i < CAN_CHANNEL_MAX; i++ )
    {
      uint32_t total_ns = pcan_device.can[i].tx_time_ns + pcan_device.can[i].rx_time_ns;
      /* get bus in percents 0 - 100% */
      pcan_device.can[i].bus_load = total_ns / (250000000u/100u);

      pcan_device.can[i].tx_time_ns = 0;
      pcan_device.can[i].rx_time_ns = 0;
    }
  }

  if( pcan_device.last_time_sync &&
      (( ts_ms - pcan_device.last_time_sync ) < PCAN_USB_CALIBRATION_INTERVAL_MS) )
    return;
  struct ucan_usb_ts_msg *pts = pcan_data_alloc_buffer( UCAN_USB_MSG_CALIBRATION, sizeof( struct ucan_usb_ts_msg ) );
  if( !pts )
    return;

  g_fw_diag.calibration_msgs_built++;
  pts->usb_frame_index = pcan_usb_frame_number();
  pts->unused = 0;
  pcan_device.last_time_sync = ts_ms;

  for( int i = 0; i < CAN_CHANNEL_MAX; i++ )
  {
    if( !(pcan_device.can[i].opt_mask & UCAN_OPTION_BUSLOAD) )
      continue;
    if( !pcan_device.can[i].bus_active )
      continue;
    struct ucan_bus_load_msg *pbs = pcan_data_alloc_buffer(  UCAN_MSG_BUSLOAD, sizeof( struct ucan_bus_load_msg ) );
    if( !pbs )
      return;

    pbs->channel = i;
    /* 0 ... 100% => 0 ... 4095 */
    pbs->bus_load = (pcan_device.can[i].bus_load*4095u)/100u;
  }
  
}
