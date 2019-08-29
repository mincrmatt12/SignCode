#include "nvic.h"
#include "pins.h"

#include "stm32f2xx.h"
#include "stm32f2xx_hal.h"
#include "stm32f2xx_hal_tim.h"

#include "srv.h"
#include "tasks/timekeeper.h"
#include "matrix.h"
#include "draw.h"
#include "fonts/lcdpixel_6.h"


extern led::Matrix<led::FrameBuffer<64, 32>> matrix;
extern srv::Servicer servicer;
extern tasks::Timekeeper timekeeper;

void nvic::init() {
	SCB->VTOR = 0x4000; // set vector table relocation to 0x4000 since that's where our image starts.
	NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

	NVIC_SetPriority(DMA2_Stream5_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(),6, 0));
	NVIC_EnableIRQ(DMA2_Stream5_IRQn);

	NVIC_SetPriority(TIM1_BRK_TIM9_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(),5, 0));
	NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn);

	NVIC_SetPriority(NVIC_SRV_TX_IRQ_NAME, NVIC_EncodePriority(NVIC_GetPriorityGrouping(),4, 0));
	NVIC_EnableIRQ(NVIC_SRV_TX_IRQ_NAME);

	NVIC_SetPriority(NVIC_SRV_RX_IRQ_NAME, NVIC_EncodePriority(NVIC_GetPriorityGrouping(),6, 0));
	NVIC_EnableIRQ(NVIC_SRV_RX_IRQ_NAME);

	NVIC_SetPriority(SysTick_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(),2, 0));
	NVIC_EnableIRQ(SysTick_IRQn);
}

extern "C" void DMA2_Stream5_IRQHandler() {
	if (LL_DMA_IsActiveFlag_TC5(DMA2)) {
		LL_DMA_ClearFlag_TC5(DMA2);
		matrix.dma_finish();
	}
}

extern "C" void TIM1_BRK_TIM9_IRQHandler() {
	if (LL_TIM_IsActiveFlag_UPDATE(TIM9)) {
		LL_TIM_ClearFlag_UPDATE(TIM9);
		matrix.tim_elapsed();
	}
}

extern "C" void NVIC_SRV_RX_IRQ_HANDLER() {
	if (NVIC_SRV_RX_ACTV(UART_DMA)) {
		NVIC_SRV_RX_CLRF(UART_DMA);
		servicer.dma_finish(true);
	}
	if (NVIC_SRV_RXE_ACTV(UART_DMA)) {
		servicer.dma_finish(true);
	}
}

extern "C" void NVIC_SRV_TX_IRQ_HANDLER() {
	if (NVIC_SRV_TX_ACTV(UART_DMA)) {
		NVIC_SRV_TX_CLRF(UART_DMA);
		servicer.dma_finish(false);
	}
}

extern "C" void SysTick_Handler() {
	timekeeper.systick_handler();
}

// hard fault handler renderer...

namespace {
	void draw_hardfault_screen(int total_loop, int remain_loop) {
		draw::fill(matrix.get_inactive_buffer(), 0, 0, 0);
		draw::text(matrix.get_inactive_buffer(), "MSign crashed!", font::lcdpixel_6::info, 0, 6, 255, 0, 0);
		draw::text(matrix.get_inactive_buffer(), "rebooting in:", font::lcdpixel_6::info, 0, 12, 128, 128, 128);

		draw::rect(matrix.get_inactive_buffer(), 0, 24, (remain_loop - total_loop) / ((total_loop + 32) / 64), 32, 100, 100, 255);
	}
}

[[noreturn]] void nvic::show_error_screen(const char * errcode) {
	// there was a hardfault... delay for a while so i know
	for (int i = 0; i < 64; ++i) {
		while (matrix.is_active()) {;}
		draw_hardfault_screen(64, i);
		draw::text(matrix.get_inactive_buffer(), errcode, font::lcdpixel_6::info, 0, 20, 255, 128, 0);
		matrix.swap_buffers();
		matrix.display();
	}

	NVIC_SystemReset();
}

extern "C" void HardFault_Handler() {
	nvic::show_error_screen("HardFault");
}
