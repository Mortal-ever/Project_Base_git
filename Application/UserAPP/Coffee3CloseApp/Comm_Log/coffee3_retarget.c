/**
  * @file      coffee3_retarget.c
  * @brief     实现 Coffee3 在 ARMCC 下的无半主机字符钩子。
  * @author    WHong
  * @date      2026-09-24
  */

/**
  * @brief  丢弃 ARM C 库终端钩子提交的字符以避免半主机调用。
  * @param[in] ch 原本会交给半主机终端的字符。
  */
void _ttywrch(int ch)
{
	(void)ch;
}
