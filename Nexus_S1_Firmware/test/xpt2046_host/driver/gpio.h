#pragma once
#define GPIO_IS_VALID_GPIO(n) ((n)>=0 && (n)<49)
#define GPIO_IS_VALID_OUTPUT_GPIO(n) (GPIO_IS_VALID_GPIO(n) && (n)!=46)
