/* generated vector source file - do not edit */
        #include "bsp_api.h"
        /* Do not build these data structures if no interrupts are currently allocated because IAR will have build errors. */
        #if VECTOR_DATA_IRQ_COUNT > 0
        BSP_DONT_REMOVE const fsp_vector_t g_vector_table[BSP_ICU_VECTOR_NUM_ENTRIES] BSP_PLACE_IN_SECTION(BSP_SECTION_APPLICATION_VECTORS) =
        {
                        [0] = sci_b_uart_rxi_isr, /* SCI9 RXI (Receive data full) */
            [1] = sci_b_uart_txi_isr, /* SCI9 TXI (Transmit data empty) */
            [2] = sci_b_uart_tei_isr, /* SCI9 TEI (Transmit end) */
            [3] = sci_b_uart_eri_isr, /* SCI9 ERI (Receive error) */
            [4] = sci_b_uart_rxi_isr, /* SCI0 RXI (Receive data full) */
            [5] = sci_b_uart_txi_isr, /* SCI0 TXI (Transmit data empty) */
            [6] = sci_b_uart_tei_isr, /* SCI0 TEI (Transmit end) */
            [7] = sci_b_uart_eri_isr, /* SCI0 ERI (Receive error) */
            [8] = iic_master_rxi_isr, /* IIC0 RXI (Receive data full) */
            [9] = iic_master_txi_isr, /* IIC0 TXI (Transmit data empty) */
            [10] = iic_master_tei_isr, /* IIC0 TEI (Transmit end) */
            [11] = iic_master_eri_isr, /* IIC0 ERI (Transfer error) */
            [12] = ceu_isr, /* CEU CEUI (CEU interrupt) */
            [13] = r_icu_isr, /* ICU IRQ29 (External pin interrupt 29) */
            [14] = rm_ethosu_isr, /* NPU IRQ (NPU IRQ) */
            [15] = adc_b_err0_isr, /* ADC ERR0 (A/D converter unit 0 Error) */
            [16] = adc_b_resovf0_isr, /* ADC RESOVF0 (A/D conversion overflow on A/D converter unit 0) */
            [17] = adc_b_calend0_isr, /* ADC CALEND0 (End of calibration of A/D converter unit 0) */
            [18] = adc_b_calend1_isr, /* ADC CALEND1 (End of calibration of A/D converter unit 1) */
            [19] = adc_b_adi0_isr, /* ADC ADI0 (End of A/D scanning operation(Gr.0)) */
            [20] = r_icu_isr, /* ICU IRQ14 (External pin interrupt 14) */
            [21] = sdhimmc_accs_isr, /* SDHIMMC0 ACCS (Card access) */
            [22] = sdhimmc_card_isr, /* SDHIMMC0 CARD (Card detect) */
            [23] = dmac_int_isr, /* DMAC0 INT (DMAC0 transfer end) */
        };
        #if BSP_FEATURE_ICU_HAS_IELSR
        const bsp_interrupt_event_t g_interrupt_event_link_select[BSP_ICU_VECTOR_NUM_ENTRIES] =
        {
            [0] = BSP_PRV_VECT_ENUM(EVENT_SCI9_RXI,GROUP0), /* SCI9 RXI (Receive data full) */
            [1] = BSP_PRV_VECT_ENUM(EVENT_SCI9_TXI,GROUP1), /* SCI9 TXI (Transmit data empty) */
            [2] = BSP_PRV_VECT_ENUM(EVENT_SCI9_TEI,GROUP2), /* SCI9 TEI (Transmit end) */
            [3] = BSP_PRV_VECT_ENUM(EVENT_SCI9_ERI,GROUP3), /* SCI9 ERI (Receive error) */
            [4] = BSP_PRV_VECT_ENUM(EVENT_SCI0_RXI,GROUP4), /* SCI0 RXI (Receive data full) */
            [5] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TXI,GROUP5), /* SCI0 TXI (Transmit data empty) */
            [6] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TEI,GROUP6), /* SCI0 TEI (Transmit end) */
            [7] = BSP_PRV_VECT_ENUM(EVENT_SCI0_ERI,GROUP7), /* SCI0 ERI (Receive error) */
            [8] = BSP_PRV_VECT_ENUM(EVENT_IIC0_RXI,GROUP0), /* IIC0 RXI (Receive data full) */
            [9] = BSP_PRV_VECT_ENUM(EVENT_IIC0_TXI,GROUP1), /* IIC0 TXI (Transmit data empty) */
            [10] = BSP_PRV_VECT_ENUM(EVENT_IIC0_TEI,GROUP2), /* IIC0 TEI (Transmit end) */
            [11] = BSP_PRV_VECT_ENUM(EVENT_IIC0_ERI,GROUP3), /* IIC0 ERI (Transfer error) */
            [12] = BSP_PRV_VECT_ENUM(EVENT_CEU_CEUI,GROUP4), /* CEU CEUI (CEU interrupt) */
            [13] = BSP_PRV_VECT_ENUM(EVENT_ICU_IRQ29,GROUP5), /* ICU IRQ29 (External pin interrupt 29) */
            [14] = BSP_PRV_VECT_ENUM(EVENT_NPU_IRQ,GROUP6), /* NPU IRQ (NPU IRQ) */
            [15] = BSP_PRV_VECT_ENUM(EVENT_ADC_ERR0,GROUP7), /* ADC ERR0 (A/D converter unit 0 Error) */
            [16] = BSP_PRV_VECT_ENUM(EVENT_ADC_RESOVF0,GROUP0), /* ADC RESOVF0 (A/D conversion overflow on A/D converter unit 0) */
            [17] = BSP_PRV_VECT_ENUM(EVENT_ADC_CALEND0,GROUP1), /* ADC CALEND0 (End of calibration of A/D converter unit 0) */
            [18] = BSP_PRV_VECT_ENUM(EVENT_ADC_CALEND1,GROUP2), /* ADC CALEND1 (End of calibration of A/D converter unit 1) */
            [19] = BSP_PRV_VECT_ENUM(EVENT_ADC_ADI0,GROUP3), /* ADC ADI0 (End of A/D scanning operation(Gr.0)) */
            [20] = BSP_PRV_VECT_ENUM(EVENT_ICU_IRQ14,GROUP4), /* ICU IRQ14 (External pin interrupt 14) */
            [21] = BSP_PRV_VECT_ENUM(EVENT_SDHIMMC0_ACCS,GROUP5), /* SDHIMMC0 ACCS (Card access) */
            [22] = BSP_PRV_VECT_ENUM(EVENT_SDHIMMC0_CARD,GROUP6), /* SDHIMMC0 CARD (Card detect) */
            [23] = BSP_PRV_VECT_ENUM(EVENT_DMAC0_INT,GROUP7), /* DMAC0 INT (DMAC0 transfer end) */
        };
        #endif
        #endif