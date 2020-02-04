/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016-2018 mesytec GmbH & Co. KG <info@mesytec.com>
 *
 * Author: Florian Lüke <f.lueke@mesytec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include "vme_config_tree.h"
#include "mvme.h"
#include "vme_config.h"
#include "vme_config_ui.h"
#include "treewidget_utils.h"
#include "mvme_stream_worker.h"
#include "vmusb.h"
#include "vme_config_scripts.h"
#include "vme_config_util.h"
#include "vme_script_editor.h"

#include <QClipboard>
#include <QDebug>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QToolButton>
#include <QTreeWidget>

using namespace std::placeholders;
using namespace vats;
using namespace mvme::vme_config;

enum NodeType
{
    NodeType_Event = QTreeWidgetItem::UserType,     // 1000
    NodeType_Module,                                // 1001
    NodeType_ModuleReset,                           // 1002
    NodeType_EventModulesInit,
    NodeType_EventReadoutLoop,
    NodeType_EventStartStop,
    NodeType_VMEScript,                             // 1006
    NodeType_Container,
};

enum DataRole
{
    DataRole_Pointer = Qt::UserRole,
    DataRole_ScriptCategory,
};

class TreeNode: public QTreeWidgetItem
{
    public:
        using QTreeWidgetItem::QTreeWidgetItem;
};

class EventNode: public TreeNode
{
    public:
        EventNode()
            : TreeNode(NodeType_Event)
        {
            setIcon(0, QIcon(":/vme_event.png"));
        }

        TreeNode *modulesNode = nullptr;
        TreeNode *readoutLoopNode = nullptr;
        TreeNode *daqStartStopNode = nullptr;
};

class ModuleNode: public TreeNode
{
    public:
        ModuleNode()
            : TreeNode(NodeType_Module)
        {}

        TreeNode *readoutNode = nullptr;
};


bool is_parent_disabled(ConfigObject *obj)
{
    Q_ASSERT(obj);

    if (auto parentConfigObject = qobject_cast<ConfigObject *>(obj->parent()))
    {
        if (!parentConfigObject->isEnabled())
            return true;

        return is_parent_disabled(parentConfigObject);
    }

    return false;
}

bool should_draw_node_disabled(QTreeWidgetItem *node)
{
    if (auto obj = Var2Ptr<ConfigObject>(node->data(0, DataRole_Pointer)))
    {
        if (!obj->isEnabled())
            return true;

        return is_parent_disabled(obj);
    }

    return false;
}


class VMEConfigTreeItemDelegate: public QStyledItemDelegate
{
    public:
        VMEConfigTreeItemDelegate(QObject* parent=0): QStyledItemDelegate(parent) {}

        virtual QWidget* createEditor(QWidget *parent,
                                      const QStyleOptionViewItem &option,
                                      const QModelIndex &index) const override
        {
            if (index.column() == 0)
            {
                return QStyledItemDelegate::createEditor(parent, option, index);
            }

            return nullptr;
        }

    protected:
        virtual void initStyleOption(QStyleOptionViewItem *option,
                                     const QModelIndex &index) const override
        {
            QStyledItemDelegate::initStyleOption(option, index);

            if (auto node = reinterpret_cast<QTreeWidgetItem *>(index.internalPointer()))
            {
                if (should_draw_node_disabled(node))
                {
                    option->state &= ~QStyle::State_Enabled;
                }
            }
        }
};

VMEConfigTreeWidget::VMEConfigTreeWidget(QWidget *parent)
    : QWidget(parent)
    , m_tree(new QTreeWidget(this))
{
    m_tree->setColumnCount(2);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setIndentation(10);
    m_tree->setItemDelegate(new VMEConfigTreeItemDelegate(this));
    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed);

    {
        auto copyShortcut = new QShortcut(QKeySequence::Copy, m_tree);
        copyShortcut->setContext(Qt::WidgetShortcut);

        connect(copyShortcut, &QShortcut::activated,
                m_tree, [this] ()
                {
                    if (auto co = getCurrentConfigObject())
                        if (canCopy(co))
                            copyToClipboard(co);
                });
    }

    {
        auto pasteShortcut = new QShortcut(QKeySequence::Paste, m_tree);
        pasteShortcut->setContext(Qt::WidgetShortcut);

        connect(pasteShortcut, &QShortcut::activated,
                m_tree, [this] ()
                {
                    if (canPaste())
                        pasteFromClipboard();
                });
    }

    auto headerItem = m_tree->headerItem();
    headerItem->setText(0, QSL("Object"));
    headerItem->setText(1, QSL("Info"));

    // Toolbar buttons
    pb_new    = make_action_toolbutton();
    pb_load   = make_action_toolbutton();
    pb_save   = make_action_toolbutton();
    pb_saveAs = make_action_toolbutton();
    //pb_notes  = make_toolbutton(QSL(":/text-document.png"), QSL("Notes"));
    //connect(pb_notes, &QPushButton::clicked, this, &VMEConfigTreeWidget::showEditNotes);

    QToolButton *pb_moreMenu = nullptr;

    {
        auto menu = new QMenu(this);
        action_showAdvanced = menu->addAction(QSL("Show advanced objects"));
        action_showAdvanced->setCheckable(true);
        connect(action_showAdvanced, &QAction::changed, this,
                &VMEConfigTreeWidget::onActionShowAdvancedChanged);

        action_dumpVMEControllerRegisters = menu->addAction(QSL("Dump VME Controller Registers"));
        connect(action_dumpVMEControllerRegisters, &QAction::triggered,
                this, &VMEConfigTreeWidget::dumpVMEControllerRegisters);
        action_dumpVMEControllerRegisters->setEnabled(false);

        auto action_exploreWorkspace = menu->addAction(QIcon(":/folder_orange.png"),
                                                       QSL("Explore Workspace"));
        connect(action_exploreWorkspace, &QAction::triggered,
                this, &VMEConfigTreeWidget::exploreWorkspace);

        pb_moreMenu = make_toolbutton(QSL(":/tree-settings.png"), QSL("More"));
        pb_moreMenu->setMenu(menu);
        pb_moreMenu->setPopupMode(QToolButton::InstantPopup);

        QSettings settings;
        action_showAdvanced->setChecked(settings.value("DAQTree/ShowAdvanced", true).toBool());
        onActionShowAdvancedChanged();
    }

    auto buttonLayout = new QHBoxLayout;
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(2);
    buttonLayout->addWidget(pb_new);
    buttonLayout->addWidget(pb_load);
    buttonLayout->addWidget(pb_save);
    buttonLayout->addWidget(pb_saveAs);
    buttonLayout->addWidget(pb_moreMenu);
    //buttonLayout->addWidget(pb_notes); TODO: implement this
    buttonLayout->addStretch(1);

    // filename label
    le_fileName = new QLineEdit;
    le_fileName->setReadOnly(true);
    auto pal = le_fileName->palette();
    pal.setBrush(QPalette::Base, QColor(239, 235, 231));
    le_fileName->setPalette(pal);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(buttonLayout);
    layout->addWidget(le_fileName);
    layout->addWidget(m_tree);

    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &VMEConfigTreeWidget::onItemDoubleClicked);
    connect(m_tree, &QTreeWidget::itemChanged, this, &VMEConfigTreeWidget::onItemChanged);
    connect(m_tree, &QTreeWidget::itemExpanded, this, &VMEConfigTreeWidget::onItemExpanded);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &VMEConfigTreeWidget::treeContextMenu);
}

void VMEConfigTreeWidget::setupActions()
{
    auto actions_ = actions();

    auto find_action = [&actions_] (const QString &name) -> QAction *
    {
        auto it = std::find_if(
            std::begin(actions_), std::end(actions_),
            [&name] (const QAction *action) {
                return action->objectName() == name;
            });

        return it != std::end(actions_) ? *it : nullptr;
    };

    pb_new->setDefaultAction(find_action("actionNewVMEConfig"));
    pb_load->setDefaultAction(find_action("actionOpenVMEConfig"));
    pb_save->setDefaultAction(find_action("actionSaveVMEConfig"));
    pb_saveAs->setDefaultAction(find_action("actionSaveVMEConfigAs"));
}

void VMEConfigTreeWidget::setConfig(VMEConfig *cfg)
{
    // Cleanup

    if (m_config)
        m_config->disconnect(this);

    // Clear the tree and the lookup mapping
    qDeleteAll(m_tree->invisibleRootItem()->takeChildren());
    m_treeMap.clear();

    m_nodeMVLCTriggerIO = nullptr;
    m_nodeDAQStart = nullptr;
    m_nodeEvents = nullptr;
    m_nodeDAQStop = nullptr;
    m_nodeManual = nullptr;


    // Recreate objects
    m_config = cfg;

    auto startScriptContainer = cfg->getGlobalObjectRoot().findChild<ContainerObject *>(
        "daq_start");
    auto stopScriptContainer = cfg->getGlobalObjectRoot().findChild<ContainerObject *>(
        "daq_stop");
    auto manualScriptContainer = cfg->getGlobalObjectRoot().findChild<ContainerObject *>(
        "manual");

    m_nodeDAQStart = addObjectNode(m_tree->invisibleRootItem(), startScriptContainer);

    m_nodeEvents = new TreeNode;
    m_nodeEvents->setText(0,  QSL("Events"));
    m_nodeEvents->setIcon(0, QIcon(":/mvme_16x16.png"));
    m_tree->addTopLevelItem(m_nodeEvents);
    m_nodeEvents->setExpanded(true);

    if (cfg)
    {
        // Repopulate events
        for (auto event: cfg->getEventConfigs())
            onEventAdded(event, false);
    }

    m_nodeDAQStop = addObjectNode(m_tree->invisibleRootItem(), stopScriptContainer);
    m_nodeManual = addObjectNode(m_tree->invisibleRootItem(), manualScriptContainer);

    if (cfg)
    {
        auto &cfg = m_config;

        connect(cfg, &VMEConfig::eventAdded,
                this, [this] (EventConfig *eventConfig) {
                    onEventAdded(eventConfig, true);
                });

        connect(cfg, &VMEConfig::eventAboutToBeRemoved,
                this, &VMEConfigTreeWidget::onEventAboutToBeRemoved);

        connect(cfg, &VMEConfig::globalScriptAdded,
                this, &VMEConfigTreeWidget::onScriptAdded);

        connect(cfg, &VMEConfig::globalScriptAboutToBeRemoved,
                this, &VMEConfigTreeWidget::onScriptAboutToBeRemoved);

        connect(cfg, &VMEConfig::modifiedChanged,
                this, &VMEConfigTreeWidget::updateConfigLabel);

        connect(cfg, &VMEConfig::vmeControllerTypeSet,
                this, &VMEConfigTreeWidget::onVMEControllerTypeSet);

        // Controller specific setup
        onVMEControllerTypeSet(cfg->getControllerType());
    }

    m_tree->resizeColumnToContents(0);
    updateConfigLabel();
}

void VMEConfigTreeWidget::onVMEControllerTypeSet(const VMEControllerType &t)
{
    if (!m_config) return;

    auto &cfg = m_config;

    delete m_nodeMVLCTriggerIO;
    m_nodeMVLCTriggerIO = nullptr;

    if (is_mvlc_controller(t))
    {
        auto mvlcTriggerIO = cfg->getGlobalObjectRoot().findChild<VMEScriptConfig *>(
            "mvlc_trigger_io");

        m_nodeMVLCTriggerIO = makeObjectNode(mvlcTriggerIO);
        m_nodeMVLCTriggerIO->setFlags(m_nodeMVLCTriggerIO->flags() & ~Qt::ItemIsEditable);
        m_treeMap[mvlcTriggerIO] = m_nodeMVLCTriggerIO;
        m_tree->insertTopLevelItem(0, m_nodeMVLCTriggerIO);
    }
}

VMEConfig *VMEConfigTreeWidget::getConfig() const
{
    return m_config;
}

template<typename T>
TreeNode *makeNode(T *data, int type = QTreeWidgetItem::Type)
{
    auto ret = new TreeNode(type);
    ret->setData(0, DataRole_Pointer, Ptr2Var(data));
    return ret;
}

TreeNode *VMEConfigTreeWidget::addScriptNode(TreeNode *parent, VMEScriptConfig* script)
{
    auto node = new TreeNode(NodeType_VMEScript);
    node->setData(0, DataRole_Pointer, Ptr2Var(script));
    node->setText(0, script->objectName());
    node->setIcon(0, QIcon(":/vme_script.png"));
    node->setFlags(node->flags() | Qt::ItemIsEditable);
    m_treeMap[script] = node;
    parent->addChild(node);

    return node;
}

TreeNode *VMEConfigTreeWidget::addEventNode(TreeNode *parent, EventConfig *event)
{
    auto eventNode = new EventNode;
    eventNode->setData(0, DataRole_Pointer, Ptr2Var(event));
    eventNode->setText(0, event->objectName());
    //eventNode->setCheckState(0, Qt::Checked);
    eventNode->setFlags(eventNode->flags() | Qt::ItemIsEditable);
    m_treeMap[event] = eventNode;
    parent->addChild(eventNode);
    eventNode->setExpanded(true);

    eventNode->modulesNode = new TreeNode(NodeType_EventModulesInit);
    auto modulesNode = eventNode->modulesNode;
    modulesNode->setText(0, QSL("Modules Init"));
    modulesNode->setIcon(0, QIcon(":/config_category.png"));
    eventNode->addChild(modulesNode);
    modulesNode->setExpanded(true);

    eventNode->readoutLoopNode = new TreeNode(NodeType_EventReadoutLoop);
    auto readoutLoopNode = eventNode->readoutLoopNode;
    readoutLoopNode->setText(0, QSL("Readout Loop"));
    readoutLoopNode->setIcon(0, QIcon(":/config_category.png"));
    eventNode->addChild(readoutLoopNode);

    {
        auto node = makeNode(event->vmeScripts["readout_start"]);
        node->setText(0, QSL("Cycle Start"));
        node->setIcon(0, QIcon(":/vme_script.png"));
        readoutLoopNode->addChild(node);
    }

    {
        auto node = makeNode(event->vmeScripts["readout_end"]);
        node->setText(0, QSL("Cycle End"));
        node->setIcon(0, QIcon(":/vme_script.png"));
        readoutLoopNode->addChild(node);
    }

    eventNode->daqStartStopNode = new TreeNode(NodeType_EventStartStop);
    auto daqStartStopNode = eventNode->daqStartStopNode;
    daqStartStopNode->setText(0, QSL("Multicast DAQ Start/Stop"));
    daqStartStopNode->setIcon(0, QIcon(":/config_category.png"));
    eventNode->addChild(daqStartStopNode);

    {
        auto node = makeNode(event->vmeScripts["daq_start"]);
        node->setText(0, QSL("DAQ Start"));
        node->setIcon(0, QIcon(":/vme_script.png"));
        daqStartStopNode->addChild(node);
    }

    {
        auto node = makeNode(event->vmeScripts["daq_stop"]);
        node->setText(0, QSL("DAQ Stop"));
        node->setIcon(0, QIcon(":/vme_script.png"));
        daqStartStopNode->addChild(node);
    }

    return eventNode;
}

TreeNode *VMEConfigTreeWidget::addModuleNodes(EventNode *parent, ModuleConfig *module)
{
    auto moduleNode = new ModuleNode;
    moduleNode->setData(0, DataRole_Pointer, Ptr2Var(module));
    moduleNode->setText(0, module->objectName());
    //moduleNode->setCheckState(0, Qt::Checked);
    moduleNode->setIcon(0, QIcon(":/vme_module.png"));
    moduleNode->setFlags(moduleNode->flags() | Qt::ItemIsEditable);
    m_treeMap[module] = moduleNode;
    parent->modulesNode->addChild(moduleNode);

    // Module reset node
    {
        auto script = module->getResetScript();
        auto node = makeNode(script, NodeType_ModuleReset);
        node->setText(0, script->objectName());
        node->setIcon(0, QIcon(":/vme_script.png"));
        moduleNode->addChild(node);
    }

    // Module init nodes
    for (auto script: module->getInitScripts())
    {
        auto node = makeNode(script);
        node->setText(0, script->objectName());
        node->setIcon(0, QIcon(":/vme_script.png"));
        moduleNode->addChild(node);
    }

    {
        auto readoutNode = makeNode(module->getReadoutScript());
        moduleNode->readoutNode = readoutNode;
        readoutNode->setText(0, module->objectName());
        readoutNode->setIcon(0, QIcon(":/vme_module.png"));

        auto readoutLoopNode = parent->readoutLoopNode;
        readoutLoopNode->insertChild(readoutLoopNode->childCount() - 1, readoutNode);
    }

    return moduleNode;
}

TreeNode *VMEConfigTreeWidget::makeObjectNode(ConfigObject *obj)
{
    int nodeType = 0;

    if (qobject_cast<EventConfig *>(obj))
        nodeType = NodeType_Event;
    if (qobject_cast<ModuleConfig *>(obj))
        nodeType = NodeType_Module;
    if (qobject_cast<VMEScriptConfig *>(obj))
        nodeType = NodeType_VMEScript;
    if (qobject_cast<ContainerObject *>(obj))
        nodeType = NodeType_Container;

    auto treeNode = new TreeNode(nodeType);

    treeNode->setData(0, DataRole_Pointer, Ptr2Var(obj));
    treeNode->setText(0, obj->objectName());

    if (obj->property("display_name").isValid())
        treeNode->setText(0, obj->property("display_name").toString());

    if (obj->property("icon").isValid())
        treeNode->setIcon(0, QIcon(obj->property("icon").toString()));

    if (auto containerObject = qobject_cast<ContainerObject *>(obj))
    {
        addContainerNodes(treeNode, containerObject);
    }

    if (auto scriptObject = qobject_cast<VMEScriptConfig *>(obj))
        treeNode->setFlags(treeNode->flags() | Qt::ItemIsEditable);

    return treeNode;
}

TreeNode *VMEConfigTreeWidget::addObjectNode(QTreeWidgetItem *parentNode, ConfigObject *obj)
{
    auto treeNode = makeObjectNode(obj);

    parentNode->addChild(treeNode);
    m_treeMap[obj] = treeNode;

    return treeNode;
}

void VMEConfigTreeWidget::addContainerNodes(QTreeWidgetItem *parent, ContainerObject *obj)
{
    for (auto child: obj->getChildren())
    {
        auto childNode = addObjectNode(parent, child);
        parent->addChild(childNode);
    }
}

void VMEConfigTreeWidget::onItemDoubleClicked(QTreeWidgetItem *item, int column)
{
    auto configObject = Var2Ptr<ConfigObject>(item->data(0, DataRole_Pointer));
    auto scriptConfig = qobject_cast<VMEScriptConfig *>(configObject);

    if (scriptConfig)
    {
        try
        {
            auto metaTag = vme_script::get_first_meta_block_tag(
                mesytec::mvme::parse(scriptConfig));

            emit editVMEScript(scriptConfig, metaTag);
            return;
        }
        catch (const vme_script::ParseError &e) { }

        emit editVMEScript(scriptConfig);
    }
}

/* Called when the contents in the column of the item change.
 * Used to implement item renaming. */
void VMEConfigTreeWidget::onItemChanged(QTreeWidgetItem *item, int column)
{
    auto obj = Var2Ptr<ConfigObject>(item->data(0, DataRole_Pointer));

    if (obj && column == 0)
    {
        if (item->flags() & Qt::ItemIsEditable)
        {
            obj->setObjectName(item->text(0));
        }

        m_tree->resizeColumnToContents(0);
    }
}

void VMEConfigTreeWidget::onItemExpanded(QTreeWidgetItem *item)
{
    m_tree->resizeColumnToContents(0);
}

void VMEConfigTreeWidget::treeContextMenu(const QPoint &pos)
{
    auto node = m_tree->itemAt(pos);
    auto obj = node ? Var2Ptr<ConfigObject>(node->data(0, DataRole_Pointer)) : nullptr;
    auto vmeScript = qobject_cast<VMEScriptConfig *>(obj);
    bool isIdle = (m_daqState == DAQState::Idle);
    bool isMVLC = is_mvlc_controller(m_config->getControllerType());
    QAction *action = nullptr;

    QMenu menu;

    //
    // Script nodes
    //
    if (vmeScript)
    {
        if (isIdle || isMVLC)
            menu.addAction(QSL("Run Script"), this, &VMEConfigTreeWidget::runScripts);
        menu.addAction(QIcon(QSL(":/pencil.png")), QSL("Edit Script"),
                       this, &VMEConfigTreeWidget::editScript);
    }

    //
    // Events
    //
    if (node == m_nodeEvents)
    {
        if (isIdle)
            menu.addAction(QSL("Add Event"), this, &VMEConfigTreeWidget::addEvent);
    }

    if (node && node->type() == NodeType_Event)
    {
        Q_ASSERT(obj);

        action = menu.addAction(QIcon(QSL(":/gear.png")), QSL("Edit Event Settings"),
                                this, &VMEConfigTreeWidget::editEventImpl);
        action->setEnabled(isIdle);

        menu.addAction(
            QIcon(QSL(":/pencil.png")), QSL("Edit Event Variables"),
            this, [this] ()
            {
                auto node = m_tree->currentItem();

                if (node && node->type() == NodeType_Event)
                {
                    auto eventConfig = Var2Ptr<EventConfig>(node->data(0, DataRole_Pointer));
                    emit editEventVariables(eventConfig);
                }
            });

        //menu.addAction(QSL("Rename Event"), this, &VMEConfigTreeWidget::editName);
        menu.addSeparator();

        action = menu.addAction(QIcon(QSL(":/list-add.png")), QSL("Add Module"),
                                this, &VMEConfigTreeWidget::addModule);
        action->setEnabled(isIdle);

    }

    if (node && node->type() == NodeType_EventModulesInit)
    {
        if (isIdle)
            menu.addAction(QSL("Add Module"), this, &VMEConfigTreeWidget::addModule);
    }

    if (node && node->type() == NodeType_Module)
    {
        Q_ASSERT(obj);

        if ((isIdle || isMVLC) && obj->isEnabled())
        {
            menu.addAction(QSL("Init Module"), this, &VMEConfigTreeWidget::initModule);
            menu.addSeparator();
            menu.addAction(QIcon(QSL(":/gear.png")), QSL("Edit Module Settings"),
                           this, &VMEConfigTreeWidget::editModule);
        }

        //menu.addAction(QSL("Rename Module"), this, &VMEConfigTreeWidget::editName);

        if (isIdle)
        {
            menu.addSeparator();
            menu.addAction(
                obj->isEnabled() ? QSL("Disable Module") : QSL("Enable Module"),
                this, [this, node]() {

                    if (isObjectEnabled(node, NodeType_Module))
                    {
                        QMessageBox::warning(
                            this,
                            QSL("Disable Module Warning"),
                            QSL("Warning: disabling the VME module that is generating the trigger"
                                " can lead to unexpected readout behavior.<br/>"
                               )
                            );
                    }

                    toggleObjectEnabled(node, NodeType_Module);
                });
        }

        if (obj->isEnabled())
        {
            menu.addAction(QSL("Show Diagnostics"), this,
                           &VMEConfigTreeWidget::handleShowDiagnostics);
        }
    }

    //
    // Global scripts
    //
    if (qobject_cast<ContainerObject *>(obj))
    {
        if (isIdle || isMVLC)
        {
            if (node->childCount() > 0)
                menu.addAction(QSL("Run scripts"), this, &VMEConfigTreeWidget::runScripts);
        }

        menu.addAction(QSL("Add script"), this, &VMEConfigTreeWidget::addGlobalScript);
    }

    if (qobject_cast<VMEScriptConfig *>(obj))
    {
        auto po = obj->parent();

        if (isIdle && po && (po->objectName() == "daq_start"
                             || po->objectName() == "daq_stop"
                             || po->objectName() == "manual"))
        {
            menu.addSeparator();
            // disabling manual scripts doesn't make any sense
            if (po->objectName() != "manual")
            {
                menu.addAction(obj->isEnabled() ? QSL("Disable Script") : QSL("Enable Script"),
                               this, [this, node]() { toggleObjectEnabled(node, NodeType_VMEScript); });
            }
        }
    }

    auto make_object_type_string = [](const ConfigObject *obj)
    {
        if (qobject_cast<const EventConfig *>(obj))
            return QSL("Event");
        if (qobject_cast<const ModuleConfig *>(obj))
            return QSL("Module");
        if (qobject_cast<const VMEScriptConfig *>(obj))
            return QSL("VME Script");
        return QString{};
    };

    // copy and paste
    {
        menu.addSeparator();

        auto action = menu.addAction(
            QIcon::fromTheme("edit-copy"), "Copy " + make_object_type_string(obj),
            [this, obj] { copyToClipboard(obj); },
            QKeySequence::Copy);

        action->setEnabled(canCopy(obj));

        QString pasteObjectTypeString;

        if (canPaste())
        {
            auto clipboardData = QGuiApplication::clipboard()->mimeData();
            auto objFromClipboard = make_object_from_mime_data_or_json_text(clipboardData);

            pasteObjectTypeString = make_object_type_string(objFromClipboard.get());
        }

        action = menu.addAction(
            QIcon::fromTheme("edit-paste"), "Paste " + pasteObjectTypeString,
            [this] { pasteFromClipboard(); },
            QKeySequence::Paste);

        action->setEnabled(canPaste());
    }

    // remove selected object
    if (isIdle)
    {
        QString objectTypeString;
        std::function<void ()> removeFunc;

        if (node && node->type() == NodeType_Event)
        {
            objectTypeString = "Event";
            removeFunc = [this] () { removeEvent(); };
        }

        if (node && node->type() == NodeType_Module)
        {
            objectTypeString = "Module";
            removeFunc = [this] () { removeModule(); };
        }

        if (qobject_cast<VMEScriptConfig *>(obj) && obj->parent())
        {
            auto parentName = obj->parent()->objectName();

            if (parentName == "daq_start"
                || parentName == "daq_stop"
                || parentName == "manual")
            {
                objectTypeString = "VME Script";
                removeFunc = [this] () { removeGlobalScript(); };
            }
        }

        if (removeFunc)
        {
            menu.addSeparator();

            menu.addAction(
                QIcon::fromTheme("edit-delete"),
                "Remove " + objectTypeString,
                removeFunc);
        }
    }

    if (!menu.isEmpty())
        menu.exec(m_tree->mapToGlobal(pos));
}

void VMEConfigTreeWidget::onEventAdded(EventConfig *eventConfig, bool expandNode)
{
    addEventNode(m_nodeEvents, eventConfig);

    for (auto module: eventConfig->getModuleConfigs())
        onModuleAdded(module);

    connect(eventConfig, &EventConfig::moduleAdded,
            this, &VMEConfigTreeWidget::onModuleAdded);

    connect(eventConfig, &EventConfig::moduleAboutToBeRemoved,
            this, &VMEConfigTreeWidget::onModuleAboutToBeRemoved);

    auto updateEventNode = [eventConfig, this](bool isModified)
    {
        auto node = static_cast<EventNode *>(m_treeMap.value(eventConfig, nullptr));

        if (!isModified || !node)
            return;

        node->setText(0, eventConfig->objectName());
        //node->setCheckState(0, eventConfig->isEnabled() ? Qt::Checked : Qt::Unchecked);

        QString infoText;

        switch (eventConfig->triggerCondition)
        {
            case TriggerCondition::Interrupt:
                {
                    infoText = QString("Trigger=IRQ%1")
                        .arg(eventConfig->irqLevel);
                } break;
            case TriggerCondition::NIM1:
                {
                    infoText = QSL("Trigger=NIM");
                } break;
            case TriggerCondition::Periodic:
                {
                    infoText = QSL("Trigger=Periodic");
                } break;
            default:
                {
                    infoText = QString("Trigger=%1")
                        .arg(TriggerConditionNames.value(eventConfig->triggerCondition));
                } break;
        }

        node->setText(1, infoText);
    };

    updateEventNode(true);

    if (expandNode)
    {
        if (auto node = m_treeMap.value(eventConfig, nullptr))
            node->setExpanded(true);
    }

    connect(eventConfig, &EventConfig::modified, this, updateEventNode);
    onActionShowAdvancedChanged();
}

void VMEConfigTreeWidget::onEventAboutToBeRemoved(EventConfig *config)
{
    for (auto module: config->getModuleConfigs())
    {
        onModuleAboutToBeRemoved(module);
    }

    delete m_treeMap.take(config);
}

void VMEConfigTreeWidget::onModuleAdded(ModuleConfig *module)
{
    auto eventNode = static_cast<EventNode *>(m_treeMap[module->parent()]);
    addModuleNodes(eventNode, module);

    auto updateModuleNodes = [module, this](bool isModified) {
        auto node = static_cast<ModuleNode *>(m_treeMap.value(module, nullptr));

        if (!isModified || !node)
            return;

        node->setText(0, module->objectName());
        node->readoutNode->setText(0, module->objectName());
        //node->setCheckState(0, module->isEnabled() ? Qt::Checked : Qt::Unchecked);

        QString infoText = QString("Type=%1, Address=0x%2")
            .arg(module->getModuleMeta().displayName)
            .arg(module->getBaseAddress(), 8, 16, QChar('0'));

        node->setText(1, infoText);
    };

    updateModuleNodes(true);

    connect(module, &ModuleConfig::modified, this, updateModuleNodes);
    onActionShowAdvancedChanged();
}

void VMEConfigTreeWidget::onModuleAboutToBeRemoved(ModuleConfig *module)
{
    auto moduleNode = static_cast<ModuleNode *>(m_treeMap[module]);
    delete moduleNode->readoutNode;
    delete m_treeMap.take(module);
}

void VMEConfigTreeWidget::onScriptAdded(VMEScriptConfig *script, const QString &category)
{
    TreeNode *parentNode = m_treeMap[script->parent()];

    qDebug() << __PRETTY_FUNCTION__ << script << script->parent() << parentNode;

    if (parentNode)
    {
        addScriptNode(parentNode, script);
        m_tree->resizeColumnToContents(0);
    }
}

void VMEConfigTreeWidget::onScriptAboutToBeRemoved(VMEScriptConfig *script)
{
    delete m_treeMap.take(script);
}

//
// Context menu action implementations
//

void VMEConfigTreeWidget::removeEvent()
{
    auto node = m_tree->currentItem();

    if (node && node->type() == NodeType_Event)
    {
        auto event = Var2Ptr<EventConfig>(node->data(0, DataRole_Pointer));
        m_config->removeEventConfig(event);
        event->deleteLater();
    }
}

void VMEConfigTreeWidget::toggleObjectEnabled(QTreeWidgetItem *node, int expectedNodeType)
{
    if (node)
    {
        qDebug() << __PRETTY_FUNCTION__ << node << node->type() << expectedNodeType
            << node->text(0);
        Q_ASSERT(node->type() == expectedNodeType);
    }

    if (node && node->type() == expectedNodeType)
    {
        if (auto obj = Var2Ptr<ConfigObject>(node->data(0, DataRole_Pointer)))
        {
            obj->setEnabled(!obj->isEnabled());
        }
    }
}

bool VMEConfigTreeWidget::isObjectEnabled(QTreeWidgetItem *node, int expectedNodeType) const
{
    if (node && node->type() == expectedNodeType)
    {
        if (auto obj = Var2Ptr<ConfigObject>(node->data(0, DataRole_Pointer)))
        {
            return obj->isEnabled();
        }
    }

    return false;
}

void VMEConfigTreeWidget::editEventImpl()
{
    auto node = m_tree->currentItem();

    if (node && node->type() == NodeType_Event)
    {
        auto eventConfig = Var2Ptr<EventConfig>(node->data(0, DataRole_Pointer));
        emit editEvent(eventConfig);
    }
}

// TODO: refactor in the same way as done to addEvent(): make it a signal and
// move the implementation elsewhere
void VMEConfigTreeWidget::addModule()
{
    auto node = m_tree->currentItem();

    while (node && node->type() != NodeType_Event)
    {
        node = node->parent();
    }

    if (node)
    {
        auto event = Var2Ptr<EventConfig>(node->data(0, DataRole_Pointer));
        bool doExpand = (event->getModuleConfigs().size() == 0);

        auto module = std::make_unique<ModuleConfig>();
        ModuleConfigDialog dialog(module.get(), event, m_config, this);
        dialog.setWindowTitle(QSL("Add Module"));
        int result = dialog.exec();

        if (result == QDialog::Accepted)
        {
            // Create and add script configs using the data stored in the
            // module meta information.
            auto moduleMeta = module->getModuleMeta();

            module->getReadoutScript()->setObjectName(moduleMeta.templates.readout.name);
            module->getReadoutScript()->setScriptContents(moduleMeta.templates.readout.contents);

            module->getResetScript()->setObjectName(moduleMeta.templates.reset.name);
            module->getResetScript()->setScriptContents(moduleMeta.templates.reset.contents);

            for (const auto &vmeTemplate: moduleMeta.templates.init)
            {
                module->addInitScript(new VMEScriptConfig(
                        vmeTemplate.name, vmeTemplate.contents));
            }

            event->addModuleConfig(module.release());

            if (doExpand)
                static_cast<EventNode *>(node)->modulesNode->setExpanded(true);
        }
    }
}

void VMEConfigTreeWidget::removeModule()
{
    auto node = m_tree->currentItem();

    while (node && node->type() != NodeType_Module)
    {
        node = node->parent();
    }

    if (node)
    {
        auto module = Var2Ptr<ModuleConfig>(node->data(0, DataRole_Pointer));
        auto event = qobject_cast<EventConfig *>(module->parent());
        if (event)
        {
            event->removeModuleConfig(module);
            module->deleteLater();
        }
    }
}

void VMEConfigTreeWidget::editModule()
{
    auto node = m_tree->currentItem();

    while (node && node->type() != NodeType_Module)
    {
        node = node->parent();
    }

    if (node)
    {
        auto moduleConfig = Var2Ptr<ModuleConfig>(node->data(0, DataRole_Pointer));
        ModuleConfigDialog dialog(moduleConfig, moduleConfig->getEventConfig(), m_config, this);
        dialog.setWindowTitle(QSL("Edit Module"));
        dialog.exec();
    }
}

void VMEConfigTreeWidget::addGlobalScript()
{
    auto node = m_tree->currentItem();
    auto obj  = Var2Ptr<ContainerObject>(node->data(0, DataRole_Pointer));
    auto category = obj->objectName();
    auto script = new VMEScriptConfig;

    script->setObjectName("new vme script");
    bool doExpand = (node->childCount() == 0);
    m_config->addGlobalScript(script, category);

    if (doExpand)
        node->setExpanded(true);

    auto scriptNode = m_treeMap.value(script, nullptr);
    assert(scriptNode);
    if (scriptNode)
    {
        m_tree->editItem(scriptNode, 0);
    }
}

void VMEConfigTreeWidget::removeGlobalScript()
{
    auto node = m_tree->currentItem();
    auto script = Var2Ptr<VMEScriptConfig>(node->data(0, DataRole_Pointer));
    m_config->removeGlobalScript(script);
}

void VMEConfigTreeWidget::runScripts()
{
    auto node = m_tree->currentItem();
    auto obj  = Var2Ptr<ConfigObject>(node->data(0, DataRole_Pointer));

    QVector<VMEScriptConfig *> scriptConfigs;

    if (auto scriptConfig = qobject_cast<VMEScriptConfig *>(obj))
    {
        scriptConfigs.push_back(scriptConfig);
    }
    else
    {
        for (int i=0; i<node->childCount(); ++i)
        {
            obj = Var2Ptr<ConfigObject>(node->child(i)->data(0, DataRole_Pointer));
            if (auto scriptConfig = qobject_cast<VMEScriptConfig *>(obj))
                scriptConfigs.push_back(scriptConfig);
        }
    }

    emit runScriptConfigs(scriptConfigs);
}

void VMEConfigTreeWidget::editScript()
{
    auto node = m_tree->currentItem();

    auto obj  = Var2Ptr<ConfigObject>(node->data(0, DataRole_Pointer));

    if (auto scriptConfig = qobject_cast<VMEScriptConfig *>(obj))
    {
        try
        {
            auto metaTag = vme_script::get_first_meta_block_tag(
                mesytec::mvme::parse(scriptConfig));

            emit editVMEScript(scriptConfig, metaTag);
            return;
        }
        catch (const vme_script::ParseError &e) { }

        emit editVMEScript(scriptConfig);
    }
}

void VMEConfigTreeWidget::editName()
{
    m_tree->editItem(m_tree->currentItem(), 0);
}

void VMEConfigTreeWidget::initModule()
{
    auto node = m_tree->currentItem();
    auto module = Var2Ptr<ModuleConfig>(node->data(0, DataRole_Pointer));
    emit runScriptConfigs(module->getInitScripts());
}

void VMEConfigTreeWidget::onActionShowAdvancedChanged()
{
    if (!m_nodeEvents) return;

    auto nodes = findItems(m_nodeEvents, [](QTreeWidgetItem *node) {
        return node->type() == NodeType_EventReadoutLoop
            || node->type() == NodeType_EventStartStop
            || node->type() == NodeType_ModuleReset;
    });

    bool showAdvanced = action_showAdvanced->isChecked();

    for (auto node: nodes)
        node->setHidden(!showAdvanced);

    QSettings settings;
    settings.setValue("DAQTree/ShowAdvanced", showAdvanced);
};

void VMEConfigTreeWidget::handleShowDiagnostics()
{
    auto node = m_tree->currentItem();
    auto module = Var2Ptr<ModuleConfig>(node->data(0, DataRole_Pointer));
    emit showDiagnostics(module);
}

void VMEConfigTreeWidget::exploreWorkspace()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_workspaceDirectory));
}

void VMEConfigTreeWidget::showEditNotes()
{
}

void VMEConfigTreeWidget::setConfigFilename(const QString &filename)
{
    m_configFilename = filename;
    updateConfigLabel();
}

void VMEConfigTreeWidget::setWorkspaceDirectory(const QString &dirname)
{
    m_workspaceDirectory = dirname;
    updateConfigLabel();
}

void VMEConfigTreeWidget::setDAQState(const DAQState &daqState)
{
    m_daqState = daqState;
}

void VMEConfigTreeWidget::setVMEControllerState(const ControllerState &state)
{
    m_vmeControllerState = state;
    action_dumpVMEControllerRegisters->setEnabled(state == ControllerState::Connected);
}

void VMEConfigTreeWidget::setVMEController(const VMEController *ctrl)
{
    m_vmeController = ctrl;
}

void VMEConfigTreeWidget::updateConfigLabel()
{
    QString fileName = m_configFilename;

    if (fileName.isEmpty())
        fileName = QSL("<not saved>");

    if (m_config && m_config->isModified())
        fileName += QSL(" *");

    auto wsDir = m_workspaceDirectory + '/';

    if (fileName.startsWith(wsDir))
    {
        fileName.remove(wsDir);
    }

    le_fileName->setText(fileName);
    le_fileName->setToolTip(fileName);
    le_fileName->setStatusTip(fileName);
}



// Note: Explicitly disallow copy/paste of ContainerObject instances for now
// until the desired semantics are clear.

bool VMEConfigTreeWidget::canCopy(const ConfigObject *obj) const
{
    if (qobject_cast<const ContainerObject *>(obj))
        return false;

    return can_mime_copy_object(obj);
}

bool VMEConfigTreeWidget::canPaste() const
{
    auto clipboardData = QGuiApplication::clipboard()->mimeData();
    auto node = m_tree->currentItem();
    auto nt = node ? node->type() : 0u;
    bool result = false;

    // First check the MIME types generated by mvme for which we allow pasting.

    if (clipboardData->hasFormat(MIMEType_JSON_VMEEventConfig))
    {
        result |= node == m_nodeEvents;
    }

    if (clipboardData->hasFormat(MIMEType_JSON_VMEModuleConfig))
    {
        result |= (nt == NodeType_Event
                   || nt == NodeType_EventModulesInit);
    }

    if (clipboardData->hasFormat(MIMEType_JSON_VMEScriptConfig))
    {
        result |= (node == m_nodeDAQStart
                   || node == m_nodeDAQStop
                   || node == m_nodeManual);
    }

    // Early return in case we already got a positive answer. Skips the more
    // expensive steps below.
    if (result)
        return result;

    // Next check json text stored in the application/json or text/plain MIME
    // types. This requires actually creating an object from the json data so
    // it's more expensive than the tests above.

    std::unique_ptr<ConfigObject> obj = make_object_from_mime_data_or_json_text(
        clipboardData);

    result |= ((node == m_nodeEvents)
               && qobject_cast<EventConfig *>(obj.get()));

    result |= ((nt == NodeType_Event
                || nt == NodeType_EventModulesInit)
               && qobject_cast<ModuleConfig *>(obj.get()));

    result |= ((node == m_nodeDAQStart
                || node == m_nodeDAQStop
                || node == m_nodeManual)
               && qobject_cast<VMEScriptConfig *>(obj.get()));

    return result;
}

ConfigObject *VMEConfigTreeWidget::getCurrentConfigObject() const
{
    if (auto node = m_tree->currentItem())
    {
        auto qobj = Var2Ptr<QObject>(node->data(0, DataRole_Pointer));
        return qobject_cast<ConfigObject *>(qobj);
    }

    return nullptr;
}

void VMEConfigTreeWidget::copyToClipboard(const ConfigObject *obj)
{
    assert(canCopy(obj));

    if (!obj || !can_mime_copy_object(obj))
        return;

    if (auto mimeData = make_mime_data(obj))
        QGuiApplication::clipboard()->setMimeData(mimeData.release());
}

void VMEConfigTreeWidget::pasteFromClipboard()
{
    if (!canPaste())
        return;

    auto clipboardData = QGuiApplication::clipboard()->mimeData();
    auto node = m_tree->currentItem();
    auto nt = node ? node->type() : 0u;
    auto destObj = node ? Var2Ptr<ConfigObject>(node->data(0, DataRole_Pointer)) : nullptr;

    auto obj = make_object_from_mime_data_or_json_text(clipboardData);

    if (!obj)
        return;

    qDebug() << __PRETTY_FUNCTION__ << "got an object from the clipboard:" << obj.get();

    generate_new_object_ids(obj.get());

    auto vmeConfig = getConfig();

    if (auto eventConfig = qobject_cast<EventConfig *>(obj.get()))
    {
        if (node != m_nodeEvents)
            return;

        eventConfig->setObjectName(make_unique_event_name("event", vmeConfig));

        // Tree is notified via VMEConfig::eventAdded()
        vmeConfig->addEventConfig(eventConfig);
        obj.release();
    }

    if (auto moduleConfig = qobject_cast<ModuleConfig *>(obj.get()))
    {
        // Direct paste onto an event node
        EventConfig *destEvent = qobject_cast<EventConfig *>(destObj);

        // Check for paste onto the "Modules Init" node of an event
        if (!destEvent
            && nt == NodeType_EventModulesInit
            && node->parent()
            && node->parent()->type() == NodeType_Event)
        {
            destEvent = Var2Ptr<EventConfig>(node->parent()->data(0, DataRole_Pointer));
        }

        if (destEvent)
        {
            moduleConfig->setObjectName(
                make_unique_module_name(moduleConfig->getModuleMeta().typeName, vmeConfig));

            // Tree is notified via EventConfig::moduleAdded()
            destEvent->addModuleConfig(moduleConfig);
            obj.release();
        }
    }

    if (auto scriptConfig = qobject_cast<VMEScriptConfig *>(obj.get()))
    {
        if (auto destContainer = qobject_cast<ContainerObject *>(destObj))
        {
            scriptConfig->setObjectName(
                make_unique_name(scriptConfig, destContainer));

            destContainer->addChild(scriptConfig);
            onScriptAdded(scriptConfig, {});
            obj.release();

            if (node)
                node->setExpanded(true);
        }
    }


    qDebug() << __PRETTY_FUNCTION__ << vmeConfig->getGlobalScriptCategories();
}
