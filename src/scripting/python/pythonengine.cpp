/* This file is part of Clementine.
   Copyright 2010, David Sansome <me@davidsansome.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <Python.h>
#include <com_trolltech_qt_core/com_trolltech_qt_core_init.h>
#include <com_trolltech_qt_gui/com_trolltech_qt_gui_init.h>
#include <com_trolltech_qt_network/com_trolltech_qt_network_init.h>

#include "pythonengine.h"
#include "pythonscript.h"
#include "core/logging.h"
#include "core/player.h"
#include "core/taskmanager.h"
#include "covers/coverproviders.h"
#include "library/library.h"
#include "library/librarybackend.h"
#include "library/libraryview.h"
#include "playlist/playlistmanager.h"
#include "radio/radiomodel.h"
#include "scripting/uiinterface.h"
#include "ui/settingsdialog.h"

#include <QFile>
#include <QtDebug>

const char* PythonEngine::kClementineModuleName = "clementine";
const char* PythonEngine::kScriptModulePrefix = "clementinescripts";
PythonEngine* PythonEngine::sInstance = NULL;


void PythonQt_init_Clementine(PyObject* module);


PythonEngine::PythonEngine(ScriptManager* manager)
  : LanguageEngine(manager),
    initialised_(false),
    modules_model_(new QStandardItemModel(this))
{
  Q_ASSERT(sInstance == NULL);
  sInstance = this;

  #ifdef Q_OS_DARWIN
    setenv("PYTHONPATH", (QCoreApplication::applicationDirPath() + "/../PlugIns").toLocal8Bit().constData(), 1);
  #endif
}

PythonEngine::~PythonEngine() {
  sInstance = NULL;

  modules_model_->clear();

  scripts_module_ = PythonQtObjectPtr();
  clementine_module_ = PythonQtObjectPtr();
  PythonQt::cleanup();
}

bool PythonEngine::EnsureInitialised() {
  if (initialised_)
    return true;

  PythonStdOut("Initialising python...");

  PythonQt::init(PythonQt::IgnoreSiteModule | PythonQt::RedirectStdOut);
  PythonQt_init_QtCore(0);
  PythonQt_init_QtGui(0);
  PythonQt_init_QtNetwork(0);
  PythonQt_init_Clementine(0);

  PythonQt* python_qt = PythonQt::self();
  python_qt->installDefaultImporter();

  connect(python_qt, SIGNAL(pythonStdOut(QString)), SLOT(PythonStdOut(QString)));
  connect(python_qt, SIGNAL(pythonStdErr(QString)), SLOT(PythonStdErr(QString)));

  // Create a clementine module
  clementine_module_ = python_qt->createModuleFromScript(kClementineModuleName);

  // Add classes
  python_qt->registerClass(&AutoExpandingTreeView::staticMetaObject, kClementineModuleName);

  const ScriptManager::GlobalData& data = manager()->data();
  if (data.valid_) {
    // Add objects
    clementine_module_.addObject("library",         data.library_->backend());
    clementine_module_.addObject("library_view",    data.library_view_);
    clementine_module_.addObject("player",          data.player_);
    clementine_module_.addObject("playlists",       data.playlists_);
    clementine_module_.addObject("radio_model",     data.radio_model_);
    clementine_module_.addObject("settings_dialog", data.settings_dialog_);
    clementine_module_.addObject("task_manager",    data.task_manager_);
    clementine_module_.addObject("cover_providers", &CoverProviders::instance());
  }

  clementine_module_.addObject("ui",           manager()->ui());
  clementine_module_.addObject("pythonengine", this);

  // Create a module for scripts
  qLog(Debug) << "Creating scripts module";
  scripts_module_ = python_qt->createModuleFromScript(kScriptModulePrefix);

  // The modules model contains all the modules
  modules_model_->clear();
  AddModuleToModel("__main__", python_qt->getMainModule());

  qLog(Debug) << "Python initialisation complete";
  initialised_ = true;
  return true;
}

Script* PythonEngine::CreateScript(const ScriptInfo& info) {
  // Initialise Python if it hasn't been done yet
  if (!EnsureInitialised()) {
    return NULL;
  }

  PythonScript* ret = new PythonScript(this, info);
  loaded_scripts_[ret->info().id()] = ret; // Used by RegisterNativeObject during startup
  if (ret->Init()) {
    AddModuleToModel(ret->module_name(), ret->module());
    return ret;
  }

  DestroyScript(ret);
  return NULL;
}

void PythonEngine::DestroyScript(Script* script) {
  PythonScript* python_script = static_cast<PythonScript*>(script);
  RemoveModuleFromModel(python_script->module_name());

  script->Unload();
  loaded_scripts_.remove(script->info().id());
  delete script;
}

void PythonEngine::PythonStdOut(const QString& str) {
  manager()->AddLogLine("Python", str, false);
}

void PythonEngine::PythonStdErr(const QString& str) {
  manager()->AddLogLine("Python", str, true);
}

void PythonEngine::AddModuleToModel(const QString& name, PythonQtObjectPtr ptr) {
  QStandardItem* item = new QStandardItem(name);
  item->setData(QVariant::fromValue(ptr));
  modules_model_->appendRow(item);
}

void PythonEngine::RemoveModuleFromModel(const QString& name) {
  foreach (QStandardItem* item, modules_model_->findItems(name)) {
    modules_model_->removeRow(item->row());
  }
}
