idf_component_register(SRCS "blu_moudle.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt
                    PRIV_REQUIRES
                        esp_ringbuf
                        nvs_flash)