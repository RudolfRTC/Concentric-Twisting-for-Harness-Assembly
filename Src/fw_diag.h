#pragma once

#include <stdint.h>

struct fw_diag_state
{
  uint32_t magic;
  uint32_t cmd_packets;
  uint32_t last_setup_request;
  uint32_t last_setup_wvalue;
  uint32_t last_setup_wlength;
  uint32_t fw_info_req_count;
  uint32_t fw_info_req_wlength;
  uint32_t cmd_reset_mode;
  uint32_t cmd_normal_mode;
  uint32_t cmd_listen_only_mode;
  uint32_t cmd_timing_slow;
  uint32_t cmd_timing_fast;
  uint32_t cmd_clk_set;
  uint32_t cmd_set_option;
  uint32_t cmd_clr_option;
  uint32_t cmd_led_set;
  uint32_t cmd_devid_set;
  uint32_t usb_out_ep1_packets;
  uint32_t usb_out_ep2_packets;
  uint32_t usb_out_ep3_packets;
  uint32_t usb_out_last_ep;
  uint32_t usb_out_last_size;
  uint32_t usb_out_last_type;
  uint32_t usb_out_last_msg_size;
  uint32_t usb_tx_msgs;
  uint32_t usb_data_in_submit_count;
  uint32_t usb_data_in_submit_bytes;
  uint32_t usb_data_in_complete_count;
  uint32_t usb_data_in_last_bytes;
  uint32_t calibration_msgs_built;
  uint32_t status_msgs_built;
  uint32_t bus_active_calls;
  uint32_t bus_open_requests;
  uint32_t bus_close_requests;
  uint32_t can_open_ok;
  uint32_t can_open_fail;
  uint32_t can_write_queued;
  uint32_t can_hw_send_attempts;
  uint32_t can_hw_send_ok;
  uint32_t can_hw_send_fail;
  uint32_t can_tx_events;
  uint32_t can_rx_fifo0;
  uint32_t can_rx_fifo1;
  uint32_t can_rx_to_protocol;
  uint32_t last_tx_id;
  uint32_t last_tx_size_flags;
  uint32_t last_can_ir;
  uint32_t last_can_psr;
};

extern volatile struct fw_diag_state g_fw_diag;
