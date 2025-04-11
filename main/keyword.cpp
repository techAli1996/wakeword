#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "sdkconfig.h"
#include "esp_task_wdt.h"
#include "led_strip.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

/*I2S Pin assignments*/
#define STD_BCLK (gpio_num_t) CONFIG_I2S_CLK_GPIO // I2S bit clock io number
#define STD_WS (gpio_num_t) CONFIG_I2S_WS_GPIO    // I2S word select io number
#define STD_DIN (gpio_num_t) CONFIG_I2S_DATA_GPIO // I2S data in io number

#define TAG "KWORD"
#define BLINK_GPIO CONFIG_BLINK_GPIO
#define LED_EVENT_RED 2
#define LED_EVENT_GREEN 1

#define CLASSIFIER_COMPUTER 0 // Label index for computer
#define CLASSIFIER_NOISE 1
#define CLASSIFIER_UNKNONE 2

#define AGC_MAX_GAIN 60.0     // Max gain multiplier
#define AGC_MIN_GAIN 1.0      // Min gain multiplier
#define TARGET_LEVEL 3000     // Target RMS level
#define AGC_STEP 1            // Gain adjustment step
#define SMOOTHING_FACTOR 0.75 // Smooth gain changes

static float gain = 30.0;

static i2s_chan_handle_t rx_chan; // I2S rx channel handler
static uint32_t *r_buf;           // I2S rx buffer

static TaskHandle_t handle_wake_word_task, handle_led_task, handle_hw_task;
static QueueHandle_t led_queue;

/*Define RGB-LED status*/
typedef struct
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    led_strip_handle_t led_strip;
    bool direction;
    uint8_t s_led_state;
} led_control_t;

/*Set LED color*/
static void set_led_rgb(led_control_t *led_control)
{
    /* If the addressable LED is enabled */
    if (led_control->s_led_state)
    {
        if (led_control->blue < 1 || led_control->blue > 20)
        {
            led_control->direction = !led_control->direction;
        }
        led_control->blue += (led_control->direction ? 1 : -1);
        led_control->red = (led_control->red > 0) ? (led_control->red - 5) : led_control->red;
        led_control->green = (led_control->green > 0) ? (led_control->green - 5) : led_control->green;
        led_strip_set_pixel(led_control->led_strip, 0, led_control->red, led_control->green, led_control->blue + 4);
        led_strip_refresh(led_control->led_strip);
    }
    else
    {
        led_strip_clear(led_control->led_strip);
    }
}

/*Configure LED strip*/
static void configure_led(led_control_t *led_control)
{
    led_control->s_led_state = 1;
    led_control->blue = 0;
    led_control->red = 0;
    led_control->green = 0;
    led_control->direction = 0;
    /* LED strip initialization with the GPIO and pixels number*/
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1, // at least one LED on board
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_control->led_strip));
    /* Set all LED off to clear all pixels */
    led_strip_clear(led_control->led_strip);
}

/*Task to control LED*/
void led_task(void *args)
{
    led_control_t led_control;
    uint8_t event;
    led_queue = xQueueCreate(5, sizeof(uint8_t));
    configure_led(&led_control);

    while (1)
    {
        if (xQueueReceive(led_queue, &event, 0) == pdTRUE)
        {
            if (event == LED_EVENT_RED)
            {
                led_control.red = 255;
            }
            else if (event == LED_EVENT_GREEN)
            {
                led_control.green = 255;
            }
            led_control.blue = 1;
            led_control.direction = 1;
        }
        set_led_rgb(&led_control);
        vTaskDelay(70 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

float adjust_gain(float current_gain, float rms_level)
{
    float gain = current_gain;

    if (rms_level > 1.5 * TARGET_LEVEL)
    {
        gain = 0.5;
    }
    else if (rms_level > TARGET_LEVEL)
    {
        gain -= AGC_STEP; // Reduce gain if too loud
    }
    else if (rms_level < TARGET_LEVEL / 2)
    {
        gain += AGC_STEP; // Increase gain if too quiet
    }

    // Clamp gain within range
    if (gain > AGC_MAX_GAIN)
        gain = AGC_MAX_GAIN;
    if (gain < AGC_MIN_GAIN)
        gain = AGC_MIN_GAIN;

    // Apply smoothing
    return (gain * (1 - SMOOTHING_FACTOR)) + (current_gain * SMOOTHING_FACTOR);
}

/*Initialize I2S microphone*/
static void i2s_init_std_simplex(void)
{
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_chan));

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EI_CLASSIFIER_FREQUENCY),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = STD_BCLK,
            .ws = STD_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = STD_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    rx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &rx_std_cfg));
}

/*Get I2S microphone data*/
static int get_signal_data(size_t offset, size_t length, float *out_ptr)
{
    float rms = 0.0;
    double sum = 0.0;
    size_t r_bytes;
    if (i2s_channel_read(rx_chan, r_buf, length * sizeof(uint32_t), &r_bytes, portMAX_DELAY) == ESP_OK)
    {
        for (size_t i = 0; i < length; i++)
        {
            int16_t sample = (int16_t)(r_buf[i] >> 16) ; // align value with int32_t, apply gain
            if ((sample * gain) > INT16_MAX || sample < -INT16_MAX)
            {
                gain = 1.0;
            };
            sample *= gain;
            out_ptr[i] = (float)sample ;
            sum += sample * sample;
        }
        rms = sqrt(sum / length);
        gain = adjust_gain(gain, rms);
        ESP_LOGD(TAG, "gain: %f", gain);
    }
    return EIDSP_OK;
}

/*Run classifier*/
static void key_word_task(void *args)
{
    // Initialize signal structure for the classifier
    signal_t signal = {
        .get_data = &get_signal_data,
        .total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE,
    };

    // Allocate buffer for audio data
    r_buf = (uint32_t *)calloc(1, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
    if (!r_buf)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for audio buffer");
        vTaskDelete(NULL);
        return;
    }

    // Initialize classifier and I2S
    run_classifier_init();
    i2s_init_std_simplex();
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    // Result variables
    ei_impulse_result_t result;
    EI_IMPULSE_ERROR ret;

    // Main processing loop
    while (1)
    {
        // Run the classifier on continuous audio data
        ret = run_classifier_continuous(&signal, &result);

        if (ret != EI_IMPULSE_OK)
        {
            ESP_LOGE(TAG, "Classifier returned %d", ret);
            continue;
        }

        // Get confidence scores
        const float computer_confidence = result.classification[CLASSIFIER_COMPUTER].value;
        const float noise_confidence = result.classification[CLASSIFIER_NOISE].value;

        // Print predictions if computer keyword detected with low confidence
        if (computer_confidence > 0.1f)
        {
            ei_printf("Predictions:\r\n");
            for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++)
            {
                ei_printf("  %s: %.5f\r\n",
                          ei_classifier_inferencing_categories[i],
                          result.classification[i].value);
            }
        }

        // Determine if we should trigger the LED
        // Only trigger if computer confidence is high and noise is low
        if (computer_confidence > 0.1f && noise_confidence < 0.1f)
        {
            uint8_t led_event = LED_EVENT_RED;
            xQueueSend(led_queue, &led_event, pdMS_TO_TICKS(2));
        }
    }

    // This code is unreachable in normal operation but included for completeness
    free(r_buf);
    vTaskDelete(NULL);
}

/*keyword main*/
extern "C" void app_main(void)
{
    xTaskCreatePinnedToCore(key_word_task, "task_read_i2s", 8192, NULL, 10, &handle_wake_word_task, 0);
    xTaskCreatePinnedToCore(led_task, "task_blink", 8192, NULL, 5, &handle_led_task, 1);
    while (1)
    {
        ESP_LOGD(TAG, "Task %s stack watermark = %u", pcTaskGetName(handle_led_task), uxTaskGetStackHighWaterMark(handle_led_task));
        ESP_LOGD(TAG, "Task %s stack watermark = %u", pcTaskGetName(handle_wake_word_task), uxTaskGetStackHighWaterMark(handle_wake_word_task));
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}
