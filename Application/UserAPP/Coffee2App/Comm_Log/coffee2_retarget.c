/**
  * @file      coffee2_retarget.c
  * @brief     Complete ARMCC no-semihosting character hooks for Coffee2.
  * @author    WHong
  * @date      2026-07-30
  */

/**
  * @brief Consume the ARM C library terminal character hook.
  * @param[in] ch Character that would otherwise use semihosting.
  */
void _ttywrch(int ch)
{
	(void)ch;
}
