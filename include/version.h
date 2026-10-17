#ifndef VERSION_H
#define VERSION_H

/* Версия прошивки.
 *
 * Формат: <этап>.<номер>
 *   - W — этап WiFi (сеть/диск/FAT32).
 *   - номер ревизии в рамках этапа (инкрементируется при каждой сборке).
 *
 * НЕ РЕДАКТИРОВАТЬ ЧИСЛО ВРУЧНУЮ — оно обновляется скриптом
 * tools/scripts/bump_version.sh при каждом make.
 */
#define FW_VERSION_MINOR 307

#define FW_VERSION_STR  "W.307"

#endif /* VERSION_H */