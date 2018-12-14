#include "srv.h"

#include <string.h>
#include "stm32f2xx_ll_bus.h"
#include "stm32f2xx_ll_usart.h"
#include "stm32f2xx_ll_gpio.h"
#include "stm32f2xx_ll_dma.h"
#include "stm32f2xx.h"

#define STATE_UNINIT 0
#define STATE_HANDSHAKE_RECV 1
#define STATE_HANDSHAKE_SENT 2
#define STATE_DMA_WAIT_SIZE 3
#define STATE_DMA_GOING 4
#define STATE_DMA_READY 5

bool srv::Servicer::important() {
	return this->state == STATE_DMA_READY || (!done());
}

bool srv::Servicer::done() {
	return (this->pending_count != 0 && !is_sending); // always runs in one loop iter
}

void srv::Servicer::loop() {
	if (this->state < STATE_DMA_WAIT_SIZE) {
		// currently doing the handshake
		switch (this->state) {
			case STATE_UNINIT:
				{
					// Send out the HANDSHAKE_INIT command.
					dma_out_buffer[0] = 0xa5;
					dma_out_buffer[1] = 0x00;
					dma_out_buffer[2] = 0x10;
					send();
					state = STATE_HANDSHAKE_RECV;
					break;
				}
			case STATE_HANDSHAKE_RECV:
				break;
			case STATE_HANDSHAKE_SENT:
				{
					// Send out the HANDSHAKE_OK command.
					dma_out_buffer[0] = 0xa5;
					dma_out_buffer[1] = 0x00;
					dma_out_buffer[2] = 0x12;
					send();

					start_recv();
					break;
				}
			default:
				break;
		}
	}
	else {
		if (this->state == STATE_DMA_READY) {
			// we are now about to process a command
			process_command();
			// ok, now do get the next one.
			start_recv();
		}
		if (this->pending_count > 0 && !is_sending) {
			uint32_t pending_operation = this->pending_operations[--this->pending_count];
			do_send_operation(pending_operation);
		}
	}
}

bool srv::Servicer::open_slot(uint16_t data_id, bool continuous, uint8_t &slot_id_out, bool persistent) {
	// first, we need to find a slot id
	int slot;
	if (persistent) {
		for (slot = 255; slot >= 0; --slot) {
			if (!this->slot_open(slot)) break;
		}
		if (slot < 0) {
			return false;
		}
	}
	else {
		for (slot = 0; slot < 256; ++slot) {
			if (!this->slot_open(slot)) break;
		}
		if (slot == 256) {
			return false;
		}
	}

	// ok, now we need to check if there's some available space in the pending buffer
	
	if (this->pending_count >= 6) {
		return false;
	}

	this->pending_operations[this->pending_count] = (continuous ? 0x00000000 : 0x01000000) | (data_id << 8) | (slot);
	++this->pending_count;

	slot_id_out = slot;
	this->slot_states[slot / 4] |= (1 << (((slot % 4) * 2) + 0)); // mark slot as open, not connected
	return true;
}

bool srv::Servicer::close_slot(uint8_t slot_id) {
	if (!this->slot_open(slot_id)) return false;

	if (this->pending_count >= 6) {
		return false;
	}

	this->pending_operations[this->pending_count] = 0x10000000 | slot_id;
	++this->pending_count;
	return true;
}

bool srv::Servicer::ack_slot(uint8_t slot_id) {
	if (!this->slot_open(slot_id)) return false;
	if (!this->slot_connected(slot_id)) return false;

	if (this->pending_count >= 6) {
		return false;
	}

	this->pending_operations[this->pending_count] = 0x20000000 | slot_id;
	++this->pending_count;
	return true;
}

inline const uint8_t * srv::Servicer::slot(uint8_t slot_id) {
	return this->slots[slot_id];
}

// packed 2-bit array, with bit 1 in each pair being connected bit 0 being open.

inline bool srv::Servicer::slot_connected(uint8_t i) {
	return (this->slot_states[i / 4] & (1 << (((i % 4) * 2) + 1))) != 0;
}

inline bool srv::Servicer::slot_open(uint8_t i) {
	return (this->slot_states[i / 4] & (1 << (((i % 4) * 2) + 0))) != 0;
}

bool srv::Servicer::ready() {
	return this->state >= STATE_DMA_WAIT_SIZE;
}

void srv::Servicer::init() {
	// Enable clocks

	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA2 | LL_AHB1_GRP1_PERIPH_GPIOG);
	LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART6);

	// Setup USART
	
	LL_USART_InitTypeDef usart_init = {0};
	usart_init.BaudRate = 115200;
	usart_init.OverSampling = LL_USART_OVERSAMPLING_16;
	usart_init.HardwareFlowControl = LL_USART_HWCONTROL_NONE;
	usart_init.DataWidth = LL_USART_DATAWIDTH_9B;
	usart_init.Parity = LL_USART_PARITY_EVEN;
	usart_init.StopBits = LL_USART_STOPBITS_1;
	usart_init.TransferDirection = LL_USART_DIRECTION_TX_RX;
	
	LL_USART_Init(USART6, &usart_init);

	LL_USART_EnableDMAReq_RX(USART6);
	LL_USART_EnableDMAReq_TX(USART6);

	LL_USART_ConfigAsyncMode(USART6);
	LL_USART_Enable(USART6);

	LL_USART_EnableDMAReq_RX(USART6);
	LL_USART_EnableDMAReq_TX(USART6);

	// Setup DMA channels

	LL_DMA_SetChannelSelection(DMA2, LL_DMA_STREAM_2, LL_DMA_CHANNEL_5); // RX
	LL_DMA_SetChannelSelection(DMA2, LL_DMA_STREAM_7, LL_DMA_CHANNEL_5); // TX

	// Config addresses doesn't actually set this.. it just reads it.

	LL_DMA_SetDataTransferDirection(DMA2, LL_DMA_STREAM_2, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
	LL_DMA_SetDataTransferDirection(DMA2, LL_DMA_STREAM_7, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
	
	LL_DMA_SetStreamPriorityLevel(DMA2, LL_DMA_STREAM_2, LL_DMA_PRIORITY_HIGH);
	LL_DMA_SetStreamPriorityLevel(DMA2, LL_DMA_STREAM_7, LL_DMA_PRIORITY_MEDIUM);

	LL_DMA_SetMode(DMA2, LL_DMA_STREAM_2, LL_DMA_MODE_NORMAL);
	LL_DMA_SetMode(DMA2, LL_DMA_STREAM_7, LL_DMA_MODE_NORMAL);

	LL_DMA_SetPeriphIncMode(DMA2, LL_DMA_STREAM_2, LL_DMA_PERIPH_NOINCREMENT);
	LL_DMA_SetMemoryIncMode(DMA2, LL_DMA_STREAM_2, LL_DMA_MEMORY_INCREMENT);
	LL_DMA_SetPeriphIncMode(DMA2, LL_DMA_STREAM_7, LL_DMA_PERIPH_NOINCREMENT);
	LL_DMA_SetMemoryIncMode(DMA2, LL_DMA_STREAM_7, LL_DMA_MEMORY_INCREMENT);

	LL_DMA_SetPeriphSize(DMA2, LL_DMA_STREAM_2, LL_DMA_PDATAALIGN_BYTE);
	LL_DMA_SetMemorySize(DMA2, LL_DMA_STREAM_2, LL_DMA_MDATAALIGN_BYTE);
	LL_DMA_SetPeriphSize(DMA2, LL_DMA_STREAM_7, LL_DMA_PDATAALIGN_BYTE);
	LL_DMA_SetMemorySize(DMA2, LL_DMA_STREAM_7, LL_DMA_MDATAALIGN_BYTE);

	LL_DMA_DisableFifoMode(DMA2, LL_DMA_STREAM_2);
	LL_DMA_DisableFifoMode(DMA2, LL_DMA_STREAM_7);

	// Setup GPIO
	
	LL_GPIO_InitTypeDef gpio_init = {0};

	gpio_init.Alternate = LL_GPIO_AF_8;
	gpio_init.Pin = LL_GPIO_PIN_9 | LL_GPIO_PIN_14;
	gpio_init.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;
	gpio_init.Mode = LL_GPIO_MODE_ALTERNATE;
	gpio_init.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
	gpio_init.Pull = LL_GPIO_PULL_UP;

	LL_GPIO_Init(GPIOG, &gpio_init);
}

void srv::Servicer::send() {
	// Send whatever is in the dma_out_buffer
	if (is_sending) return;
	is_sending = true;

	LL_DMA_ConfigAddresses(DMA2, LL_DMA_STREAM_7, 
			(uint32_t)(dma_out_buffer),
			LL_USART_DMA_GetRegAddr(USART6),
			LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
	LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_7, dma_out_buffer[1] + 3);

	LL_DMA_DisableIT_HT(DMA2, LL_DMA_STREAM_7);
	LL_DMA_EnableIT_TC(DMA2, LL_DMA_STREAM_7);
	LL_DMA_EnableIT_TE(DMA2, LL_DMA_STREAM_7);
	
	LL_USART_Enable(USART6);
	LL_USART_ClearFlag_TC(USART6);
	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_7);

	LL_USART_EnableDMAReq_TX(USART6);
}

void srv::Servicer::start_recv() {
	state = STATE_DMA_WAIT_SIZE;
	// setup a receieve of 3 bytes

	LL_DMA_ConfigAddresses(DMA2, LL_DMA_STREAM_2, 
			LL_USART_DMA_GetRegAddr(USART6),
			(uint32_t)(this->dma_buffer),
			LL_DMA_DIRECTION_PERIPH_TO_MEMORY);

	LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_2, 3);

	LL_DMA_EnableIT_TC(DMA2, LL_DMA_STREAM_2);
	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_2);
}

void srv::Servicer::recv_full() {
	state = STATE_DMA_GOING;
	// setup a recieve similar to out_buffer

	LL_DMA_ConfigAddresses(DMA2, LL_DMA_STREAM_2, 
			LL_USART_DMA_GetRegAddr(USART6),
			(uint32_t)(&this->dma_buffer[3]),
			LL_DMA_DIRECTION_PERIPH_TO_MEMORY);

	LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_2, dma_buffer[1]);

	LL_DMA_EnableIT_TC(DMA2, LL_DMA_STREAM_2);
	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_2);
}

void srv::Servicer::do_send_operation(uint32_t operation) {
	// switch on the operation
	uint8_t op = (operation >> 16) & 0xFF;
	uint32_t param = operation & 0xFFFFFF;

	switch (op) {
		case 0x00:
		case 0x01:
			{
				// send an OPEN_CONN command
				bool cont = op == 0x01;
				dma_out_buffer[0] = 0xa5;
				dma_out_buffer[1] = 0x04;
				dma_out_buffer[2] = 0x20;

				dma_out_buffer[3] = (cont ? 0x01 : 0x00);
				dma_out_buffer[4] = (param & 0x0000FF);
				dma_out_buffer[5] = (param & 0x00FF00) >> 8;
				dma_out_buffer[6] = (param & 0xFF0000) >> 16;
				send();
			}
			break;
		case 0x10:
			{
				// send a CLOSE_CONN command
				dma_out_buffer[0] = 0xa5;
				dma_out_buffer[1] = 0x01;
				dma_out_buffer[2] = 0x21;

				dma_out_buffer[3] = param & 0x0000FF;
				send();
			}
			break;
		case 0x20:
			{
				// send a NEW_DATA command
				dma_out_buffer[0] = 0xa5;
				dma_out_buffer[1] = 0x01;
				dma_out_buffer[2] = 0x22;

				dma_out_buffer[3] = param & 0x0000FF;
				send();
			}
			break;

	}
}

void srv::Servicer::process_command() {
	// determine what to do
    if (dma_buffer[0] != 0xa6) {
		// a6 means from the esp
		return;
	}

	switch (dma_buffer[2]) {
		case 0x30:
			{
				// service an acknowledged open command (connected)
				// ACK_OPEN_CONN
				uint8_t slot = dma_buffer[3];

				this->slot_states[slot / 4] |= (1 << (((slot % 4) * 2) + 1)); // mark slot as connected
			}
			break;
		case 0x31:
			{
				// service a ACK_CLOSE_CONN.
				uint8_t slot = dma_buffer[3];

				this->slot_states[slot / 4] &= ~(0b11 << ((slot % 4)*2)); // mark the slot as unopen and disconnected
			}
			break;
		case 0x40:
			{
				// service a SLOT_DATA command
				//
				// contains d0 = slot_id
				// contains d1-d(size-1) data
				//
				// memset(the array, 0)
				uint8_t slot = dma_buffer[3];
				uint8_t len = dma_buffer[1] - 1;

				memset(slots[slot], 0, 16);
				memcpy(slots[slot], &dma_buffer[4], len);
			}
			break;
		// TODO: notif
	}
}

void srv::Servicer::dma_finish(bool incoming) {
	if (incoming) {
		LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_2);
		// first, check if we need to handle the handshake command
		if (state == STATE_HANDSHAKE_RECV) {
			if (dma_buffer[0] == 0xa6 && dma_buffer[1] == 0x00 && dma_buffer[2] == 0x01) {
				state = STATE_HANDSHAKE_SENT;
				return;
			}
		}
		else if (state == STATE_DMA_WAIT_SIZE) {
			recv_full();
		}
		else {
			state = STATE_DMA_READY;
		}
	}
	else {
		LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_7);

		while (!LL_USART_IsActiveFlag_TC(USART6)) {
			;
		}

		is_sending = false;
	}
}
