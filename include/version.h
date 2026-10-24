#ifndef VERSION_H
#define VERSION_H

/* Версия прошивки.
 *
 * Формат: WU.<major>.<minor>
 *   - major — крупный этап (сейчас 1 — эмуляторы);
 *   - minor инкрементируется при каждой сборке через make
 *     (tools/scripts/bump_version.sh), при переполнении 999 -> major+1, minor=0.
 *
 * Точка старта новой нумерации — WU.1.000 (первый работающий Lynx).
 */
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 13

#define FW_VERSION_STR  "WU.1.013"

#endif /* VERSION_H */