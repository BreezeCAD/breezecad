#pragma once
// Управление бюджетом времени булевой операции — без включения заголовков
// OCCT: этот заголовок можно подключать из platform/app слоёв.

namespace bz::kernel::occt {

/// A boolean stopped by its wall-clock budget: thrown across our own frames.
struct BooleanBudgetExceeded {};

/// Wall-clock budget of one boolean, ms (<= 0: no limit, as before).
/// On exceeding, the operation is cancelled and comes back an error: the
/// part opens with a note on the operation instead of hanging.
void   SetBooleanBudgetMs(double ms);
double BooleanBudgetMs();

/// Пока детали сборки грузятся на своих потоках, параллельная пара сборок
/// пересечений внутри одной булевой выключена, чтобы пулы OCCT не
/// перемножались; вложенность регистрируется Push/Pop.
void PushBooleansSerial();
void PopBooleansSerial();

/// Лимит времени одной операции по умолчанию, мс (настройка «Прерывать зависшие»).
inline constexpr double kDefaultBooleanBudgetMs = 20000.0;

} // namespace bz::kernel::occt
