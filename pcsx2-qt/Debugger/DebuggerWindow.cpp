// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebuggerWindow.h"

#include "AsyncDialogs.h"
#include "Debugger/DebuggerView.h"
#include "Debugger/Docking/DockManager.h"

#include "DebugTools/DebugInterface.h"
#include "DebugTools/Breakpoints.h"
#include "DebugTools/DebuggerControl.h"
#include "DebugTools/MIPSAnalyst.h"
#include "DebugTools/MipsStackWalk.h"
#include "DebugTools/SymbolImporter.h"
#include "QtHost.h"
#include "MainWindow.h"
#include "AnalysisOptionsDialog.h"

DebuggerWindow* g_debugger_window = nullptr;

DebuggerWindow::DebuggerWindow(QWidget* parent)
	: KDDockWidgets::QtWidgets::MainWindow(QStringLiteral("DebuggerWindow"), {}, parent)
	, m_dock_manager(new DockManager(this))
{
	m_ui.setupUi(this);

	g_debugger_window = this;

	setupDefaultToolBarState();
	setupFonts();
	restoreWindowGeometry();

	m_dock_manager->loadLayouts();

	connect(m_ui.actionAnalyse, &QAction::triggered, this, &DebuggerWindow::onAnalyse);
	connect(m_ui.actionSettings, &QAction::triggered, this, &DebuggerWindow::onSettings);
	connect(m_ui.actionGameSettings, &QAction::triggered, this, &DebuggerWindow::onGameSettings);
	connect(m_ui.actionClose, &QAction::triggered, this, &DebuggerWindow::close);

	connect(m_ui.actionOnTop, &QAction::triggered, this, [this](bool checked) {
		if (checked)
			setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
		else
			setWindowFlags(windowFlags() & ~Qt::WindowStaysOnTopHint);
		show();
	});

	connect(m_ui.actionRun, &QAction::triggered, this, &DebuggerWindow::onRunPause);
	connect(m_ui.actionStepInto, &QAction::triggered, this, &DebuggerWindow::onStepInto);
	connect(m_ui.actionStepOver, &QAction::triggered, this, &DebuggerWindow::onStepOver);
	connect(m_ui.actionStepOut, &QAction::triggered, this, &DebuggerWindow::onStepOut);

	connect(m_ui.actionShutDown, &QAction::triggered, [this]() {
		if (currentCPU() && currentCPU()->isAlive())
			g_emu_thread->shutdownVM(false);
	});

	connect(m_ui.actionReset, &QAction::triggered, [this]() {
		if (currentCPU() && currentCPU()->isAlive())
			g_emu_thread->resetVM();
	});

	connect(m_ui.menuTools, &QMenu::aboutToShow, this, [this]() {
		m_dock_manager->createToolsMenu(m_ui.menuTools);
	});

	connect(m_ui.menuWindows, &QMenu::aboutToShow, this, [this]() {
		m_dock_manager->createWindowsMenu(m_ui.menuWindows);
	});

	connect(m_ui.actionResetAllLayouts, &QAction::triggered, this, [this]() {
		const QString title = tr("Confirmation");
		const QString text = tr("Are you sure you want to reset all layouts?");

		AsyncDialogs::question(this, title, text, [this]() {
			m_dock_manager->resetAllLayouts();
		});
	});

	connect(m_ui.actionResetDefaultLayouts, &QAction::triggered, this, [this]() {
		const QString title = tr("Confirmation");
		const QString text = tr("Are you sure you want to reset the default layouts?");

		AsyncDialogs::question(this, title, text, [this]() {
			m_dock_manager->resetDefaultLayouts();
		});
	});

	connect(g_emu_thread, &EmuThread::onVMPaused, this, []() {
		DebuggerView::broadcastEvent(DebuggerEvents::VMUpdate());
	});

	connect(g_emu_thread, &EmuThread::onVMStarting, this, &DebuggerWindow::onVMStarting);
	connect(g_emu_thread, &EmuThread::onVMPaused, this, &DebuggerWindow::onVMPaused);
	connect(g_emu_thread, &EmuThread::onVMResumed, this, &DebuggerWindow::onVMResumed);
	connect(g_emu_thread, &EmuThread::onVMStopped, this, &DebuggerWindow::onVMStopped);

	if (QtHost::IsVMValid())
	{
		onVMStarting();

		if (QtHost::IsVMPaused())
			onVMPaused();
		else
			onVMResumed();
	}
	else
	{
		onVMStopped();
	}

	m_dock_manager->switchToLayout(0);

	QMenuBar* menu_bar = menuBar();

	setMenuWidget(m_dock_manager->createMenuBar(menu_bar));

	connect(m_dock_manager, &DockManager::focusedViewForNavigationChanged,
		this, &DebuggerWindow::updateNavigationButtons);

	m_ui.actionNavigateBack->setEnabled(false);
	m_ui.actionNavigateForward->setEnabled(false);

	connect(m_ui.actionNavigateBack, &QAction::triggered, this, [this]() {
		DebuggerView* view = m_dock_manager->focusedViewForNavigation();
		if (!view)
			return;

		view->navigateBack();
	});

	connect(m_ui.actionNavigateForward, &QAction::triggered, this, [this]() {
		DebuggerView* view = m_dock_manager->focusedViewForNavigation();
		if (!view)
			return;

		view->navigateForward();
	});

	updateTheme();

	Host::RunOnCPUThread([]() {
		R5900SymbolImporter.OnDebuggerOpened();
	});

	updateFromSettings();
}

DebuggerWindow* DebuggerWindow::getInstance()
{
	if (!g_debugger_window)
		createInstance();

	return g_debugger_window;
}

DebuggerWindow* DebuggerWindow::createInstance()
{
	// Setup KDDockWidgets.
	DockManager::configureDockingSystem();

	if (g_debugger_window)
		destroyInstance();

	return new DebuggerWindow(nullptr);
}

void DebuggerWindow::destroyInstance()
{
	if (g_debugger_window)
		g_debugger_window->close();
}

bool DebuggerWindow::shouldShowOnStartup()
{
	return Host::GetBaseBoolSettingValue("Debugger/UserInterface", "ShowOnStartup", false);
}

DockManager& DebuggerWindow::dockManager()
{
	return *m_dock_manager;
}

void DebuggerWindow::setupDefaultToolBarState()
{
	// Hiding all the toolbars lets us save the default state of the window with
	// all the toolbars hidden. The DockManager will show the appropriate ones
	// later anyway.
	for (QToolBar* toolbar : findChildren<QToolBar*>())
		toolbar->hide();

	m_default_toolbar_state = saveState();

	for (QToolBar* toolbar : findChildren<QToolBar*>())
		connect(toolbar, &QToolBar::topLevelChanged, m_dock_manager, &DockManager::updateToolBarLockState);
}

void DebuggerWindow::clearToolBarState()
{
	restoreState(m_default_toolbar_state);
}

void DebuggerWindow::setupFonts()
{
	m_font_size = Host::GetBaseIntSettingValue("Debugger/UserInterface", "FontSize", QApplication::font().pointSize());
	if (m_font_size < MINIMUM_FONT_SIZE || m_font_size > MAXIMUM_FONT_SIZE)
		m_font_size = QApplication::font().pointSize();

	m_ui.actionIncreaseFontSize->setShortcuts(QKeySequence::ZoomIn);
	connect(m_ui.actionIncreaseFontSize, &QAction::triggered, this, [this]() {
		if (m_font_size >= MAXIMUM_FONT_SIZE)
			return;

		m_font_size++;

		updateFontActions();
		updateTheme();
		saveFontSize();
	});

	m_ui.actionDecreaseFontSize->setShortcut(QKeySequence::ZoomOut);
	connect(m_ui.actionDecreaseFontSize, &QAction::triggered, this, [this]() {
		if (m_font_size <= MINIMUM_FONT_SIZE)
			return;

		m_font_size--;

		updateFontActions();
		updateTheme();
		saveFontSize();
	});

	connect(m_ui.actionResetFontSize, &QAction::triggered, this, [this]() {
		m_font_size = QApplication::font().pointSize();

		updateFontActions();
		updateTheme();
		saveFontSize();
	});

	updateFontActions();
}

void DebuggerWindow::updateFontActions()
{
	m_ui.actionIncreaseFontSize->setEnabled(m_font_size < MAXIMUM_FONT_SIZE);
	m_ui.actionDecreaseFontSize->setEnabled(m_font_size > MINIMUM_FONT_SIZE);
	m_ui.actionResetFontSize->setEnabled(m_font_size != QApplication::font().pointSize());
}

void DebuggerWindow::saveFontSize()
{
	Host::SetBaseIntSettingValue("Debugger/UserInterface", "FontSize", m_font_size);
	Host::CommitBaseSettingChanges();
}

int DebuggerWindow::fontSize()
{
	return m_font_size;
}

void DebuggerWindow::updateTheme()
{
	// Detect recursive StyleChange events caused by updating the stylesheet.
	if (m_is_updating_theme)
		return;

	m_is_updating_theme = true;

	// TODO: Migrate away from stylesheets to improve performance.
	setStyleSheet(QString("font-size: %1pt;").arg(m_font_size));

	// HACK: Improve performance for the default font size setting. It seems we
	// need to call setStyleSheet twice here otherwise some widgets do not
	// update properly.
	if (m_font_size == QApplication::font().pointSize())
		setStyleSheet(QString());

	dockManager().updateTheme();

	m_is_updating_theme = false;
}

void DebuggerWindow::saveWindowGeometry()
{
	std::string old_geometry = Host::GetBaseStringSettingValue("Debugger/UserInterface", "WindowGeometry");

	std::string geometry;
	if (shouldSaveWindowGeometry())
		geometry = saveGeometry().toBase64().toStdString();

	if (geometry != old_geometry)
	{
		Host::SetBaseStringSettingValue("Debugger/UserInterface", "WindowGeometry", geometry.c_str());
		Host::CommitBaseSettingChanges();
	}
}

void DebuggerWindow::restoreWindowGeometry()
{
	if (!shouldSaveWindowGeometry())
		return;

	std::string geometry = Host::GetBaseStringSettingValue("Debugger/UserInterface", "WindowGeometry");
	restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(geometry)));
}

bool DebuggerWindow::shouldSaveWindowGeometry()
{
	return Host::GetBaseBoolSettingValue("Debugger/UserInterface", "SaveWindowGeometry", true);
}

void DebuggerWindow::updateFromSettings()
{
	const int refresh_interval = Host::GetBaseIntSettingValue("Debugger/UserInterface", "RefreshInterval", 1000);
	const int effective_refresh_interval = std::clamp(refresh_interval, 10, 100000);

	if (!m_refresh_timer)
	{
		m_refresh_timer = new QTimer(this);
		connect(m_refresh_timer, &QTimer::timeout, this, []() {
			DebuggerView::broadcastEvent(DebuggerEvents::Refresh());
		});
		m_refresh_timer->start(effective_refresh_interval);
	}
	else
	{
		m_refresh_timer->setInterval(effective_refresh_interval);
	}
}

void DebuggerWindow::updateNavigationButtons()
{
	DebuggerView* view = m_dock_manager->focusedViewForNavigation();
	if (view)
	{
		m_ui.actionNavigateBack->setEnabled(view->canNavigateBack());
		m_ui.actionNavigateForward->setEnabled(view->canNavigateForward());
	}
	else
	{
		m_ui.actionNavigateBack->setEnabled(false);
		m_ui.actionNavigateForward->setEnabled(false);
	}
}

void DebuggerWindow::onVMStarting()
{
	m_ui.actionRun->setEnabled(true);
	m_ui.actionStepInto->setEnabled(true);
	m_ui.actionStepOver->setEnabled(true);
	m_ui.actionStepOut->setEnabled(true);

	m_ui.actionAnalyse->setEnabled(true);
	m_ui.actionGameSettings->setEnabled(true);

	m_ui.actionShutDown->setEnabled(true);
	m_ui.actionReset->setEnabled(true);
}

void DebuggerWindow::onVMPaused()
{
	m_ui.actionRun->setText(tr("Run"));
	m_ui.actionRun->setIcon(QIcon::fromTheme(QStringLiteral("play-line")));
	m_ui.actionStepInto->setEnabled(true);
	m_ui.actionStepOver->setEnabled(true);
	m_ui.actionStepOut->setEnabled(true);

	// DebuggerControl::OnVMPaused has already cleared the temporary breakpoints, reset the
	// triggered flag and set skip-first, so the CBreakPoints state read here is spent. Ask
	// it what stopped us instead. That bookkeeping moved into the core because none of it
	// ran when this window was closed.
	const DebuggerControl::StopEvent stop = DebuggerControl::GetLastStop();
	if (stop.reason == DebuggerControl::StopReason::Breakpoint ||
		stop.reason == DebuggerControl::StopReason::Step)
	{
		// Select a layout tab corresponding to the CPU that triggered the breakpoint and
		// make it start blinking, unless the breakpoint came from stepping.
		const bool blink_tab = stop.reason != DebuggerControl::StopReason::Step;
		m_dock_manager->switchToLayoutWithCPU(stop.cpu, blink_tab);
	}

	// Stops us from telling the disassembly view to jump somwhere because
	// breakpoint code paused the core.
	if (!CBreakPoints::GetCorePaused())
		emit onVMActuallyPaused();
	else
		CBreakPoints::SetCorePaused(false);
}

void DebuggerWindow::onVMResumed()
{
	m_ui.actionRun->setText(tr("Pause"));
	m_ui.actionRun->setIcon(QIcon::fromTheme(QStringLiteral("pause-line")));
	m_ui.actionStepInto->setEnabled(false);
	m_ui.actionStepOver->setEnabled(false);
	m_ui.actionStepOut->setEnabled(false);
}

void DebuggerWindow::onVMStopped()
{
	m_ui.actionRun->setEnabled(false);
	m_ui.actionStepInto->setEnabled(false);
	m_ui.actionStepOver->setEnabled(false);
	m_ui.actionStepOut->setEnabled(false);

	m_ui.actionAnalyse->setEnabled(false);
	m_ui.actionGameSettings->setEnabled(false);

	m_ui.actionShutDown->setEnabled(false);
	m_ui.actionReset->setEnabled(false);
}

void DebuggerWindow::onAnalyse()
{
	AnalysisOptionsDialog* dialog = new AnalysisOptionsDialog(this);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}

void DebuggerWindow::onSettings()
{
	g_main_window->doSettings("Debug");
}

void DebuggerWindow::onGameSettings()
{
	g_main_window->doGameSettings("Debug");
}

void DebuggerWindow::onRunPause()
{
	g_emu_thread->setVMPaused(!QtHost::IsVMPaused());
}

void DebuggerWindow::onStepInto()
{
	// The branch, delay-slot and stack-walk logic this used to carry now lives in
	// DebuggerControl, so the debug server steps exactly the way this window does.
	step(DebuggerControl::StepMode::Into);
}

void DebuggerWindow::onStepOver()
{
	step(DebuggerControl::StepMode::Over);
}

void DebuggerWindow::onStepOut()
{
	step(DebuggerControl::StepMode::Out);
}

void DebuggerWindow::step(DebuggerControl::StepMode mode)
{
	DebugInterface* cpu = currentCPU();
	if (!cpu)
		return;

	const BreakPointCpu cpu_type = cpu->getCpuType();
	Host::RunOnCPUThread([cpu_type, mode] { DebuggerControl::Step(cpu_type, mode); });

	update();
}

void DebuggerWindow::changeEvent(QEvent* event)
{
	if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
		updateTheme();
}

void DebuggerWindow::closeEvent(QCloseEvent* event)
{
	dockManager().saveCurrentLayout();
	saveWindowGeometry();

	Host::RunOnCPUThread([]() {
		R5900SymbolImporter.OnDebuggerClosed();
	});

	KDDockWidgets::QtWidgets::MainWindow::closeEvent(event);

	g_debugger_window = nullptr;
	deleteLater();
}

DebugInterface* DebuggerWindow::currentCPU()
{
	std::optional<BreakPointCpu> maybe_cpu = m_dock_manager->cpu();
	if (!maybe_cpu.has_value())
		return nullptr;

	return &DebugInterface::get(*maybe_cpu);
}

#include "moc_DebuggerWindow.cpp"
