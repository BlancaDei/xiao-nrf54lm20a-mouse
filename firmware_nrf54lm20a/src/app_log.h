#pragma once

/* 正式版默认关闭周期性/事件级调试日志，只保留错误与启动信息。
 * 需要抓包排查时，把这里改成 1 重新编译即可。 */
#ifndef APP_VERBOSE_LOG
#define APP_VERBOSE_LOG 0
#endif
