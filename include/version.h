#ifndef VERSION_H
#define VERSION_H

/* Версия прошивки.
 *
 * Формат: W.<minor>
 *   - minor инкрементируется при каждой сборке через make (tools/scripts/bump_version.sh).
 */
#define FW_VERSION_MINOR 492

#define FW_VERSION_STR  "W.492"

#endif /* VERSION_H */