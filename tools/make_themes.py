#!/usr/bin/env python3
"""Generate the MTPlugins themes: themes/<Name>.qss, themes/images/<name>/*.png and themes/<Name>.colors.
Usage: python3 tools/make_themes.py  (needs Pillow, writes into themes/, a renamed or dropped theme goes into kRetired in plugins/common/themeicons.cpp)"""
import colorsys
import os
from PIL import Image, ImageDraw

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "themes")

PALETTES = {
    "Dark": dict(   # grey, blue accent, light blue icons
        bg="#202225", panel="#2a2c30", base="#1a1b1e", alt="#222428", border="#3a3d42",
        text="#dcdde0", dim="#9a9ea6", disabled="#62666d", accent="#3b8eea", accent2="#5aa2f0",
        sel="#2f6db5", seltext="#ffffff", hover="#33363b", button="#33363b", buttonhover="#3d4046",
        buttonpress="#26282c", tip="#2f3237", tiptext="#ececec", handle="#4a4e55", handlehover="#5d626a",
        mark="#ffffff", icons="#8fbcf2"),
    "Treyarch": dict(   # radiant\stylesheet.qss colours on the modern shapes, Radiant's own icons and default view colours
        bg="#323232", panel="#323232", base="#242424", alt="#2b2b2b", border="#1e1e1e",
        text="#b1b1b1", dim="#b1b1b1", disabled="#505050", accent="#ffa02f", accent2="#ffaa00",
        sel="#d7801a", seltext="#000000", hover="#3d3d3d", button="#4e4e4e", buttonhover="#565656",
        buttonpress="#292929", tip="#ffa02f", tiptext="#000000", handle="#b1b1b1", handlehover="#ffa02f",
        mark="#000000", icons="#b1b1b1", css="treyarch", own_icons=True, colors=False),
    "Midnight": dict(   # deep navy, teal accent, aqua icons
        bg="#0d1424", panel="#131c31", base="#090e1a", alt="#0e1424", border="#26355a",
        text="#d3ddf2", dim="#8e97aa", disabled="#586072", accent="#2fc4b2", accent2="#54cfc0",
        sel="#17736a", seltext="#ffffff", hover="#212839", button="#1b2742", buttonhover="#2a3650",
        buttonpress="#121a2e", tip="#1d263b", tiptext="#d3ddf2", handle="#3b496c", handlehover="#515f80",
        mark="#000000", icons="#7ee3d6"),
    "Ember": dict(   # the darkest: near black and warm, amber
        bg="#171614", panel="#201e1b", base="#11100f", alt="#181614", border="#34312c",
        text="#e9e4da", dim="#a09c95", disabled="#67645f", accent="#e8962f", accent2="#eca954",
        sel="#9b5b16", seltext="#ffffff", hover="#2c2b28", button="#2a2723", buttonhover="#393632",
        buttonpress="#1e1c19", tip="#2a2825", tiptext="#e9e4da", handle="#4a4641", handlehover="#615e58",
        mark="#000000", icons="#f4b25e"),
    "Plum": dict(   # dark purple, orchid accent, lilac icons
        bg="#221726", panel="#2c1e31", base="#1a111d", alt="#221726", border="#47324e",
        text="#eadff0", dim="#a499a9", disabled="#6e6373", accent="#d966c4", accent2="#e082cf",
        sel="#8a3a7d", seltext="#ffffff", hover="#362b3a", button="#35253b", buttonhover="#433449",
        buttonpress="#281b2c", tip="#36283b", tiptext="#eadff0", handle="#5b4761", handlehover="#705d76",
        mark="#ffffff", icons="#e2a8f0"),
    "Forest": dict(   # deep green, lime accent, mint icons
        bg="#14201a", panel="#1a2a22", base="#0f1813", alt="#14201a", border="#2c4436",
        text="#dbe9df", dim="#95a39a", disabled="#606c65", accent="#8fd14f", accent2="#a3d96f",
        sel="#3f7a2a", seltext="#ffffff", hover="#28342e", button="#21332a", buttonhover="#304238",
        buttonpress="#18261e", tip="#24342b", tiptext="#dbe9df", handle="#41584a", handlehover="#586d60",
        mark="#000000", icons="#a6e3a1"),
    "Figglebottom": dict(   # dark brown and tan, every icon is the dog (tools/figglebottom.png)
        bg="#231813", panel="#2d201a", base="#19110d", alt="#211712", border="#4f3b2d",
        text="#f1e3cf", dim="#ab9c8b", disabled="#70645a", accent="#d39c5c", accent2="#dfb27d",
        sel="#8c5f30", seltext="#ffffff", hover="#372a22", button="#3a2a20", buttonhover="#49372b",
        buttonpress="#2a1e17", tip="#33261e", tiptext="#f1e3cf", handle="#614b3b", handlehover="#79614e",
        mark="#000000", icons="#d9a56a", pack="figglebottom.png"),
    "Slate": dict(   # the medium one: blue-grey, coral accent, peach icons
        bg="#4a5160", panel="#545c6c", base="#3e4450", alt="#484f5d", border="#6b7486",
        text="#eef0f5", dim="#b5b8c1", disabled="#888d99", accent="#ff8c6b", accent2="#ffa186",
        sel="#c4573b", seltext="#ffffff", hover="#5a616f", button="#5b6475", buttonhover="#676f7f",
        buttonpress="#4c5462", tip="#5c6373", tiptext="#eef0f5", handle="#7b8393", handlehover="#8c93a2",
        mark="#000000", icons="#ffc4ad"),
    "Light": dict(   # white, blue accent, deep blue icons
        bg="#f0f0f0", panel="#e4e4e4", base="#ffffff", alt="#f6f6f6", border="#c2c2c2",
        text="#1e1f22", dim="#5c6068", disabled="#a4a7ad", accent="#2f6db5", accent2="#3b8eea",
        sel="#3b78c4", seltext="#ffffff", hover="#d8d8d8", button="#e2e2e2", buttonhover="#d5d5d5",
        buttonpress="#c6c6c6", tip="#ffffff", tiptext="#1e1f22", handle="#b9b9b9", handlehover="#9d9d9d",
        mark="#ffffff", icons="#1f4f8f"),
    "Sage": dict(   # green-grey, green accent, deep green icons
        bg="#cfd8d0", panel="#c0ccc2", base="#e8eee8", alt="#d6dfd7", border="#9aab9d",
        text="#1b261f", dim="#5a645d", disabled="#8b948d", accent="#2f8550", accent2="#549b70",
        sel="#3f9461", seltext="#ffffff", hover="#bdc6be", button="#bccabe", buttonhover="#afbdb1",
        buttonpress="#a9b6ab", tip="#e8eee8", tiptext="#1b261f", handle="#8b9b8e", handlehover="#7a8a7e",
        mark="#ffffff", icons="#2b5e3d"),
    "Paper": dict(   # sepia, terracotta accent, brown icons
        bg="#ede0c6", panel="#e2d1b1", base="#f9f1e0", alt="#efe3cb", border="#c4ac83",
        text="#33281a", dim="#746856", disabled="#a69a85", accent="#ad4f22", accent2="#bc6f4a",
        sel="#bc6331", seltext="#ffffff", hover="#daceb5", button="#ddcaa6", buttonhover="#cfbd9b",
        buttonpress="#c9b795", tip="#f9f1e0", tiptext="#33281a", handle="#b39c76", handlehover="#a08b69",
        mark="#ffffff", icons="#7a3a17"),
    "Rose": dict(   # pink, crimson accent, wine icons
        bg="#f5e4e8", panel="#ecd3da", base="#fff6f8", alt="#f6e6ea", border="#d6aab6",
        text="#3a1f27", dim="#7b646b", disabled="#ae999f", accent="#c2335e", accent2="#cd587b",
        sel="#c94a72", seltext="#ffffff", hover="#e2d0d5", button="#ecd0d8", buttonhover="#dec2ca",
        buttonpress="#d7bbc3", tip="#fff6f8", tiptext="#3a1f27", handle="#c399a5", handlehover="#af8792",
        mark="#ffffff", icons="#8a2443"),
    "Steam Classic": dict(   # Steam 2004 / CS 1.6 (VGUI2): olive green, gold text, bevelled square widgets
        bg="#4c5844", panel="#4c5844", base="#3e4637", alt="#434b3b", border="#292c21",
        text="#d8ded3", dim="#a0aa95", disabled="#7e8873", accent="#c4b550", accent2="#d6c867",
        sel="#958831", seltext="#ffffff", hover="#56634d", button="#4c5844", buttonhover="#4c5844",
        buttonpress="#434d3c", tip="#3e4637", tiptext="#d8ded3", handle="#4c5844", handlehover="#56634d",
        mark="#d8ded3", icons="#d8ded3", lite="#8c9284", arrows="triangle", css="bevel"),
    "Steam Modern": dict(   # today's Steam client: slate panels, #3d4450 buttons, bright blue
        bg="#23262e", panel="#171a21", base="#1a1d23", alt="#1e2128", border="#32363f",
        text="#dcdedf", dim="#8b929a", disabled="#5c6168", accent="#1a9fff", accent2="#4cb0ff",
        sel="#3d4450", seltext="#ffffff", hover="#2c3039", button="#3d4450", buttonhover="#dcdedf",
        buttonpress="#1a9fff", tip="#3d4450", tiptext="#ffffff", handle="#3d4450", handlehover="#5a6270",
        mark="#ffffff", icons="#dcdedf", css="steam"),
    "Blender": dict(   # Blender 2.8+: charcoal regions, mid grey buttons, the blue active state, dark menus
        bg="#303030", panel="#282828", base="#1d1d1d", alt="#2b2b2b", border="#3a3a3a",
        text="#e6e6e6", dim="#a6a6a6", disabled="#707070", accent="#4772b3", accent2="#5680c2",
        sel="#4772b3", seltext="#ffffff", hover="#3d3d3d", button="#545454", buttonhover="#656565",
        buttonpress="#4772b3", tip="#181818", tiptext="#e6e6e6", handle="#4d4d4d", handlehover="#6b6b6b",
        mark="#ffffff", icons="#e6e6e6", css="blender"),
    "Maya": dict(   # Autodesk Maya: its mid grey, dark fields, flat lighter buttons, steel blue selection
        bg="#444444", panel="#3a3a3a", base="#2b2b2b", alt="#323232", border="#2a2a2a",
        text="#c8c8c8", dim="#9a9a9a", disabled="#6e6e6e", accent="#5285a6", accent2="#6a9cbd",
        sel="#5285a6", seltext="#ffffff", hover="#525252", button="#5d5d5d", buttonhover="#707070",
        buttonpress="#2b2b2b", tip="#2b2b2b", tiptext="#c8c8c8", handle="#5d5d5d", handlehover="#707070",
        mark="#ffffff", icons="#c8c8c8", css="maya"),
}

# extra rules after the TEMPLATE for a palette with css=<name>
EXTRA_CSS = {
    # VGUI2 bevels: raised = light top/left, dark bottom/right (lite, border), sunken the other way round
    "bevel": """
QToolTip {{ border: 1px solid {lite}; }}
QMenuBar {{ border-bottom: 1px solid {border}; }}
QMenuBar::item:selected {{ background-color: {sel}; color: {seltext}; }}
QMenu {{ border: 1px solid; border-color: {lite} {border} {border} {lite}; }}
QToolButton {{ border-radius: 0px; }}
QToolButton:hover {{ background-color: transparent; border-color: {lite} {border} {border} {lite}; }}
QToolButton:pressed, QToolButton:checked {{ background-color: {buttonpress}; border-color: {border} {lite} {lite} {border}; }}
QTabWidget::pane {{ border: 1px solid; border-color: {lite} {border} {border} {lite}; }}
QTabBar::tab {{ background-color: {alt}; color: {dim}; border: 1px solid; border-color: {lite} {border} {border} {lite}; }}
QTabBar::tab:top, QTabBar::tab:bottom {{ border-radius: 0px; }}
QTabBar::tab:top {{ border-bottom: none; }}
QTabBar::tab:bottom {{ border-top: none; }}
QTabBar::tab:selected {{ background-color: {bg}; color: {accent}; }}
QTabBar::tab:top:selected {{ border-top: 1px solid {lite}; }}
QTabBar::tab:bottom:selected {{ border-bottom: 1px solid {border}; }}
QTabBar::tab:hover:!selected {{ background-color: {alt}; color: {text}; }}
QPushButton {{ border: 1px solid; border-color: {lite} {border} {border} {lite}; border-radius: 0px; }}
QPushButton:hover {{ color: {accent}; border-color: {lite} {border} {border} {lite}; }}
QPushButton:pressed, QPushButton:checked {{ background-color: {buttonpress}; color: {accent}; border-color: {border} {lite} {lite} {border}; }}
QPushButton:default {{ border-color: {lite} #000000 #000000 {lite}; }}
QPushButton:disabled {{ background-color: {bg}; border-color: {lite} {border} {border} {lite}; }}
QLineEdit, QTextEdit, QPlainTextEdit, QAbstractSpinBox, QComboBox {{ border: 1px solid; border-color: {border} {lite} {lite} {border}; border-radius: 0px; }}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QAbstractSpinBox:focus, QComboBox:hover, QComboBox:on {{ border-color: {border} {lite} {lite} {border}; }}
QComboBox {{ background-color: {base}; }}
QComboBox:disabled {{ background-color: {bg}; }}
QComboBox::drop-down {{ background-color: {button}; border: 1px solid; border-color: {lite} {border} {border} {lite}; }}
QComboBox::drop-down:on {{ border-color: {border} {lite} {lite} {border}; }}
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {{ border: 1px solid; border-color: {lite} {border} {border} {lite}; }}
QAbstractSpinBox::up-button:pressed, QAbstractSpinBox::down-button:pressed {{ border-color: {border} {lite} {lite} {border}; }}
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {{ border-radius: 0px; }}
QCheckBox::indicator, QAbstractItemView::indicator {{ border: 1px solid; border-color: {border} {lite} {lite} {border}; border-radius: 0px; }}
QRadioButton::indicator {{ border: 1px solid; border-color: {border} {lite} {lite} {border}; }}
QCheckBox::indicator:hover, QRadioButton::indicator:hover, QAbstractItemView::indicator:hover {{ border-color: {border} {accent} {accent} {border}; }}
QCheckBox::indicator:checked, QAbstractItemView::indicator:checked, QCheckBox::indicator:indeterminate, QAbstractItemView::indicator:indeterminate, QRadioButton::indicator:checked {{ background-color: {base}; border-color: {border} {lite} {lite} {border}; }}
QCheckBox::indicator:checked:disabled {{ background-color: {bg}; }}
QGroupBox {{ border: 1px solid; border-color: {border} {lite} {lite} {border}; border-radius: 0px; }}
QGroupBox::title {{ color: {text}; }}
QAbstractItemView {{ border: 1px solid; border-color: {border} {lite} {lite} {border}; }}
QHeaderView::section {{ background-color: {bg}; color: {text}; border: 1px solid; border-color: {lite} {border} {border} {lite}; }}
QHeaderView::section:hover {{ background-color: {bg}; color: {accent}; }}
QScrollBar:vertical {{ background-color: {alt}; width: 16px; margin: 16px 0px 16px 0px; }}
QScrollBar:horizontal {{ background-color: {alt}; height: 16px; margin: 0px 16px 0px 16px; }}
QScrollBar::handle {{ background-color: {bg}; border: 1px solid; border-color: {lite} {border} {border} {lite}; border-radius: 0px; }}
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {{ margin: 0px; }}
QScrollBar::handle:hover {{ background-color: {handlehover}; }}
QScrollBar::add-line, QScrollBar::sub-line {{ background-color: {bg}; border: 1px solid; border-color: {lite} {border} {border} {lite}; subcontrol-origin: margin; }}
QScrollBar::add-line:pressed, QScrollBar::sub-line:pressed {{ border-color: {border} {lite} {lite} {border}; }}
QScrollBar::add-line:vertical {{ height: 14px; width: 14px; subcontrol-position: bottom; }}
QScrollBar::sub-line:vertical {{ height: 14px; width: 14px; subcontrol-position: top; }}
QScrollBar::add-line:horizontal {{ width: 14px; height: 14px; subcontrol-position: right; }}
QScrollBar::sub-line:horizontal {{ width: 14px; height: 14px; subcontrol-position: left; }}
QScrollBar::up-arrow {{ image: url(images/{key}/arrow-up.png); width: 8px; height: 8px; }}
QScrollBar::down-arrow {{ image: url(images/{key}/arrow-down.png); width: 8px; height: 8px; }}
QScrollBar::left-arrow {{ image: url(images/{key}/arrow-left.png); width: 8px; height: 8px; }}
QScrollBar::right-arrow {{ image: url(images/{key}/arrow-right.png); width: 8px; height: 8px; }}
QSlider::groove:horizontal, QSlider::groove:vertical {{ background-color: {base}; border: 1px solid; border-color: {border} {lite} {lite} {border}; border-radius: 0px; }}
QSlider::sub-page:horizontal, QSlider::add-page:vertical {{ background-color: {sel}; border-radius: 0px; }}
QSlider::handle:horizontal {{ width: 8px; margin: -6px 0px; }}
QSlider::handle:vertical {{ height: 8px; margin: 0px -6px; }}
QSlider::handle:horizontal, QSlider::handle:vertical {{ background-color: {bg}; border: 1px solid; border-color: {lite} {border} {border} {lite}; border-radius: 0px; }}
QSlider::handle:hover {{ background-color: {handlehover}; }}
QProgressBar {{ background-color: {base}; border: 1px solid; border-color: {border} {lite} {lite} {border}; border-radius: 0px; }}
QProgressBar::chunk {{ background-color: {accent}; width: 6px; margin: 1px; }}
QSplitter::handle:hover, QMainWindow::separator:hover {{ background-color: {lite}; }}
QStatusBar {{ border-top: 1px solid {lite}; }}
""",
    # the Steam client: grey buttons that go light on hover, green default button, blue pressed gradient
    "steam": """
QMenuBar {{ background-color: {panel}; border-bottom: none; }}
QMenuBar::item {{ color: #b8bcbf; padding: 5px 10px; }}
QMenuBar::item:selected, QMenuBar::item:pressed {{ background-color: transparent; color: #ffffff; }}
QMenu {{ background-color: {button}; border: none; padding: 4px 0px; }}
QMenu::item {{ color: {text}; }}
QMenu::item:selected {{ background-color: {text}; color: {bg}; }}
QMenu::separator {{ background-color: #4c5361; }}
QToolButton {{ border-radius: 2px; }}
QToolButton:hover {{ background-color: {button}; border-color: {button}; }}
QToolButton:checked {{ background-color: {accent}; border-color: {accent}; }}
QPushButton {{ background-color: {button}; color: {text}; border: none; border-radius: 2px; padding: 5px 16px; }}
QPushButton:hover {{ background-color: {buttonhover}; color: {bg}; }}
QPushButton:pressed, QPushButton:checked {{ background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #47bfff, stop:1 #1a44c2); color: #ffffff; }}
QPushButton:default {{ background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #75b022, stop:1 #588a1b); color: #ffffff; }}
QPushButton:default:hover {{ background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #8ed629, stop:1 #6aa621); }}
QPushButton:disabled {{ background-color: {hover}; color: {disabled}; }}
QLineEdit, QTextEdit, QPlainTextEdit, QAbstractSpinBox {{ background-color: #32353c; border: 1px solid #32353c; border-radius: 3px; }}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QAbstractSpinBox:focus {{ border-color: {accent}; }}
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {{ background-color: transparent; border-left: none; }}
QComboBox {{ background-color: {button}; border: none; border-radius: 2px; }}
QComboBox:hover, QComboBox:on {{ background-color: #464d58; }}
QComboBox::drop-down {{ border-left: none; }}
QComboBox QAbstractItemView {{ background-color: {button}; border: none; selection-background-color: {text}; selection-color: {bg}; }}
QTabBar::tab {{ background-color: transparent; border: none; color: {dim}; padding: 6px 12px; }}
QTabBar::tab:selected {{ color: #ffffff; }}
QTabBar::tab:top:selected {{ border-top: none; border-bottom: 2px solid {accent}; }}
QTabBar::tab:bottom:selected {{ border-bottom: none; border-top: 2px solid {accent}; }}
QTabBar::tab:hover:!selected {{ background-color: transparent; color: {text}; }}
QTabWidget::pane {{ border: none; border-top: 1px solid {border}; }}
QAbstractItemView::item:hover {{ background-color: {hover}; }}
QAbstractItemView::item:selected {{ background-color: {button}; color: #ffffff; }}
QCheckBox::indicator, QAbstractItemView::indicator {{ background-color: {button}; border: 1px solid {button}; border-radius: 2px; }}
QRadioButton::indicator {{ background-color: {button}; border: 1px solid {button}; }}
QCheckBox::indicator:hover, QRadioButton::indicator:hover, QAbstractItemView::indicator:hover {{ border-color: {dim}; }}
QGroupBox {{ border: none; border-top: 1px solid {button}; border-radius: 0px; margin-top: 18px; padding-top: 6px; }}
QGroupBox::title {{ color: {dim}; left: 0px; padding: 0px 4px 0px 0px; }}
QHeaderView::section {{ background-color: {bg}; color: {dim}; border-color: {panel}; }}
QDockWidget::title {{ background-color: {panel}; border-bottom: none; }}
QStatusBar {{ background-color: {panel}; border-top: none; }}
QSlider::groove:horizontal {{ height: 4px; background-color: {button}; }}
QSlider::groove:vertical {{ width: 4px; background-color: {button}; }}
QSlider::handle:horizontal, QSlider::handle:vertical {{ background-color: {text}; }}
QProgressBar {{ background-color: {button}; border: none; border-radius: 2px; }}
QProgressBar::chunk {{ background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1a44c2, stop:1 #47bfff); border-radius: 2px; }}
QScrollBar:vertical {{ background-color: transparent; width: 10px; }}
QScrollBar:horizontal {{ background-color: transparent; height: 10px; }}
QScrollBar::handle {{ border-radius: 3px; }}
""",
    # Blender: rounded grey buttons, dark fields, blue for whatever is on, near black menus
    "blender": """
QPushButton {{ background-color: {button}; border: 1px solid {border}; border-radius: 4px; padding: 3px 12px; }}
QPushButton:hover {{ background-color: {buttonhover}; border-color: {border}; }}
QPushButton:pressed, QPushButton:checked {{ background-color: {accent}; border-color: {accent}; color: #ffffff; }}
QPushButton:default {{ border-color: {accent}; }}
QPushButton:disabled {{ background-color: {hover}; }}
QToolButton {{ border-radius: 4px; }}
QToolButton:hover {{ background-color: {button}; border-color: {button}; }}
QToolButton:checked {{ background-color: {accent}; border-color: {accent}; }}
QLineEdit, QTextEdit, QPlainTextEdit {{ background-color: {base}; border: 1px solid {base}; border-radius: 4px; }}
QAbstractSpinBox {{ background-color: {button}; border: 1px solid {border}; border-radius: 4px; }}
QAbstractSpinBox:focus {{ background-color: {base}; }}
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {{ background-color: transparent; border-left: none; }}
QComboBox {{ background-color: {panel}; border: 1px solid {border}; border-radius: 4px; }}
QComboBox::drop-down {{ border-left: none; }}
QMenuBar {{ background-color: {panel}; border-bottom: none; }}
QMenu {{ background-color: #181818; border: 1px solid {panel}; }}
QMenu::item:selected {{ background-color: {accent}; }}
QMenu::separator {{ background-color: #2d2d2d; }}
QTabBar::tab {{ background-color: {base}; border-color: {base}; color: #9c9c9c; }}
QTabBar::tab:top {{ border-top-left-radius: 4px; border-top-right-radius: 4px; }}
QTabBar::tab:selected {{ background-color: {bg}; border-color: {bg}; color: #ffffff; }}
QTabBar::tab:top:selected {{ border-top: 1px solid {bg}; }}
QTabBar::tab:bottom:selected {{ border-bottom: 1px solid {bg}; }}
QAbstractItemView {{ background-color: #282828; alternate-background-color: #2b2b2b; }}
QAbstractItemView::item:selected {{ background-color: #334d80; color: #ffffff; }}
QCheckBox::indicator, QAbstractItemView::indicator {{ background-color: {button}; border: 1px solid {button}; border-radius: 3px; }}
QRadioButton::indicator {{ background-color: {button}; border: 1px solid {button}; }}
QCheckBox::indicator:hover, QRadioButton::indicator:hover, QAbstractItemView::indicator:hover {{ background-color: {buttonhover}; border-color: {buttonhover}; }}
QCheckBox::indicator:checked, QAbstractItemView::indicator:checked, QRadioButton::indicator:checked {{ background-color: {accent}; border-color: {accent}; }}
QGroupBox {{ background-color: #3d3d3d; border: none; border-radius: 4px; margin-top: 18px; padding-top: 6px; }}
QGroupBox::title {{ color: {text}; left: 4px; }}
QHeaderView::section {{ background-color: {bg}; color: {text}; }}
QSlider::groove:horizontal {{ height: 6px; background-color: {button}; border-radius: 3px; }}
QSlider::groove:vertical {{ width: 6px; background-color: {button}; border-radius: 3px; }}
QSlider::sub-page:horizontal, QSlider::add-page:vertical {{ border-radius: 3px; }}
QSlider::handle:horizontal, QSlider::handle:vertical {{ background-color: #d9d9d9; }}
QProgressBar {{ background-color: {button}; border: none; border-radius: 4px; }}
QProgressBar::chunk {{ border-radius: 4px; }}
QScrollBar:vertical {{ width: 10px; }}
QScrollBar:horizontal {{ height: 10px; }}
QScrollBar::handle {{ border-radius: 4px; }}
""",
    # Maya: flat lighter buttons, dark fields, group boxes headed like its frame layouts, steel blue selection
    "maya": """
QPushButton {{ background-color: {button}; border: none; border-radius: 2px; padding: 4px 12px; }}
QPushButton:hover {{ background-color: {buttonhover}; }}
QPushButton:pressed, QPushButton:checked {{ background-color: {sel}; color: #ffffff; }}
QPushButton:default {{ background-color: #686868; }}
QPushButton:disabled {{ background-color: #4d4d4d; }}
QToolButton {{ border-radius: 2px; }}
QToolButton:hover {{ background-color: {button}; border-color: {button}; }}
QToolButton:checked {{ background-color: {sel}; border-color: {sel}; }}
QLineEdit, QTextEdit, QPlainTextEdit, QAbstractSpinBox {{ background-color: {base}; border: 1px solid {base}; border-radius: 2px; }}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QAbstractSpinBox:focus {{ border-color: {accent}; }}
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {{ background-color: {button}; border-left: none; }}
QComboBox {{ background-color: {button}; border: none; border-radius: 2px; }}
QComboBox:hover {{ background-color: {buttonhover}; }}
QComboBox::drop-down {{ border-left: none; }}
QMenuBar {{ background-color: {bg}; border-bottom: none; }}
QMenu {{ background-color: {bg}; border: 1px solid {base}; }}
QMenu::item:selected {{ background-color: {sel}; }}
QTabBar::tab {{ background-color: {panel}; border-color: {base}; }}
QTabBar::tab:top, QTabBar::tab:bottom {{ border-radius: 0px; }}
QTabBar::tab:selected {{ background-color: {button}; color: #ffffff; }}
QTabBar::tab:top:selected, QTabBar::tab:bottom:selected {{ border-top: 1px solid {base}; border-bottom: 1px solid {base}; }}
QCheckBox::indicator, QAbstractItemView::indicator {{ background-color: {base}; border: 1px solid {base}; border-radius: 2px; }}
QRadioButton::indicator {{ background-color: {base}; border: 1px solid {base}; }}
QCheckBox::indicator:checked, QAbstractItemView::indicator:checked, QCheckBox::indicator:indeterminate, QAbstractItemView::indicator:indeterminate, QRadioButton::indicator:checked {{ background-color: {base}; border-color: {base}; }}
QCheckBox::indicator:hover, QRadioButton::indicator:hover, QAbstractItemView::indicator:hover {{ border-color: {buttonhover}; }}
QGroupBox {{ border: 1px solid {panel}; border-radius: 2px; margin-top: 22px; padding-top: 4px; }}
QGroupBox::title {{ subcontrol-origin: margin; left: 0px; padding: 3px 10px; background-color: {button}; color: #e0e0e0; border-radius: 2px; }}
QHeaderView::section {{ background-color: {button}; color: {text}; border-color: {bg}; }}
QSlider::groove:horizontal {{ height: 4px; background-color: {base}; border-radius: 0px; }}
QSlider::groove:vertical {{ width: 4px; background-color: {base}; border-radius: 0px; }}
QSlider::sub-page:horizontal, QSlider::add-page:vertical {{ background-color: {base}; }}
QSlider::handle:horizontal {{ width: 8px; margin: -6px 0px; border-radius: 1px; background-color: #bdbdbd; }}
QSlider::handle:vertical {{ height: 8px; margin: 0px -6px; border-radius: 1px; background-color: #bdbdbd; }}
QProgressBar {{ background-color: {base}; border: none; border-radius: 2px; }}
QProgressBar::chunk {{ background-color: {sel}; }}
QScrollBar::handle {{ border-radius: 2px; }}
""",
    # Treyarch: the stock sheet's colours and gradients over the template's shapes
    "treyarch": """
QToolTip {{ border-color: #000000; }}
QMenuBar::item {{ border: 1px solid transparent; }}
QMenuBar::item:selected {{ background: transparent; border-color: #ffaa00; }}
QMenuBar::item:pressed {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0.4 #343434, stop:1 #212121); color: {text}; border-color: #000000; }}
QMenu {{ border-color: #000000; }}
QMenu::item:selected {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ffa02f, stop:1 #d7801a); color: #000000; }}
QMenu::separator {{ background-color: #424242; }}
QMainWindow::separator {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #161616, stop:0.5 #151515, stop:0.6 #212121, stop:1 #343434); }}
QMainWindow::separator:hover {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #d7801a, stop:0.5 #b56c17, stop:1 #ffa02f); }}
QScrollArea {{ background-color: #212121; }}
QToolButton:hover {{ background-color: transparent; border-color: #ffaa00; }}
QToolButton:pressed {{ background-color: transparent; color: #ffa02f; }}
QToolButton:checked {{ background-color: transparent; border-color: #995500; color: {text}; }}
QDockWidget::title {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #424242, stop:0.5 #242424, stop:1 #323232); }}
QDockWidget::close-button, QDockWidget::float-button {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #d7801a, stop:0.5 #b56c17, stop:1 #ffa02f); }}
QDockWidget::close-button:hover, QDockWidget::float-button:hover {{ background-color: #ffa02f; }}
QDockWidget::close-button:pressed, QDockWidget::float-button:pressed {{ background-color: #424242; }}
QTabWidget::pane, QTabBar::tab {{ border-color: #444444; }}
QTabBar::tab {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0.4 #343434, stop:1 #212121); color: #b1b1b1; }}
QTabBar::tab:selected {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #444444, stop:0.4 #444444, stop:1 #313131); color: #b1b1b1; }}
QTabBar::tab:top:selected {{ border-top-color: #ffaa00; }}
QTabBar::tab:bottom:selected {{ border-bottom-color: #ffaa00; }}
QTabBar::tab:hover:!selected {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ffaa00, stop:0.1 #ffaa00, stop:0.11 #343434, stop:0.4 #343434, stop:1 #212121); }}
QPushButton, QComboBox, QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #565656, stop:0.1 #525252, stop:0.5 #4e4e4e, stop:0.9 #4a4a4a, stop:1 #464646); border-color: #1e1e1e; }}
QPushButton:hover, QComboBox:hover {{ border-color: #ffa02f; }}
QPushButton:pressed, QPushButton:checked, QComboBox:on, QAbstractSpinBox::up-button:pressed, QAbstractSpinBox::down-button:pressed {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #2d2d2d, stop:0.1 #2b2b2b, stop:0.5 #292929, stop:0.9 #282828, stop:1 #252525); color: #ffa02f; }}
QPushButton:disabled, QComboBox:disabled {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1d1d1d, stop:0.1 #1b1b1b, stop:0.5 #191919, stop:0.9 #181818, stop:1 #151515); }}
QLineEdit, QAbstractSpinBox, QComboBox:editable {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #646464, stop:1 #5d5d5d); border-color: #1e1e1e; }}
QTextEdit, QPlainTextEdit {{ background-color: #242424; }}
QAbstractItemView {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #4d4d4d, stop:0.1 #646464, stop:1 #5d5d5d); alternate-background-color: #5d5d5d; }}
QAbstractItemView::item:hover, QAbstractItemView::item:selected {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ffa02f, stop:1 #d7801a); color: #000000; }}
QComboBox QAbstractItemView {{ border-color: #a9a9a9; selection-background-color: #ffaa00; }}
QHeaderView, QTableCornerButton::section {{ background-color: #505050; }}
QHeaderView::section {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #616161, stop:0.5 #505050, stop:0.6 #434343, stop:1 #656565); color: #ffffff; border-color: #6c6c6c; }}
QHeaderView::section:hover {{ color: #ffffff; }}
QCheckBox::indicator, QRadioButton::indicator, QAbstractItemView::indicator {{ background-color: #323232; border-color: #b1b1b1; }}
QCheckBox::indicator:hover, QRadioButton::indicator:hover, QAbstractItemView::indicator:hover {{ border-color: #ffaa00; }}
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled {{ border-color: #444444; }}
QCheckBox:disabled {{ color: #414141; }}
QScrollBar:vertical {{ background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #121212, stop:0.2 #282828, stop:1 #484848); }}
QScrollBar:horizontal {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #121212, stop:0.2 #282828, stop:1 #484848); }}
QScrollBar::handle:vertical, QScrollBar::handle:vertical:hover {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ffa02f, stop:0.5 #d7801a, stop:1 #ffa02f); }}
QScrollBar::handle:horizontal, QScrollBar::handle:horizontal:hover {{ background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #ffa02f, stop:0.5 #d7801a, stop:1 #ffa02f); }}
QSlider::groove:horizontal {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #121212, stop:0.2 #282828, stop:1 #484848); }}
QSlider::groove:vertical {{ background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #121212, stop:0.2 #282828, stop:1 #484848); }}
QSlider::handle:horizontal, QSlider::handle:vertical, QSlider::handle:hover {{ background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #d7801a, stop:0.5 #b56c17, stop:1 #ffa02f); }}
QProgressBar {{ border-color: #808080; }}
QProgressBar::chunk {{ background-color: #d7801a; }}
""",
}

# logos={app: (letters, fill, ink)}: a rounded square with letters and an inset border. App "" is Radiant (logo.png),
# the others logo-<app>.png. Needs Segoe UI Semibold, elsewhere the committed PNGs are kept.
LOGO_FONTS = [r"C:\Windows\Fonts\seguisb.ttf", "/mnt/c/Windows/Fonts/seguisb.ttf"]


# the launcher's toolbar icon of each program, swapped for its logo by the icon pack
LOGO_BUTTONS = {"": "resources/Radiant.png", "ape": "resources/AssetEditor.png", "launcher": "resources/ModLauncher.png"}


def write_logo(path, letters, fill, ink, n=256):
    from PIL import ImageFont
    font = next((f for f in LOGO_FONTS if os.path.exists(f)), None)
    if not font:
        return os.path.exists(path)
    s = 4
    N = n * s
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    g = ImageDraw.Draw(img)
    r = N * 0.2
    g.rounded_rectangle([0, 0, N - 1, N - 1], radius=r, fill=hexrgb(fill))
    inset, w = N * 0.055, max(1, int(N * 0.028))
    g.rounded_rectangle([inset, inset, N - 1 - inset, N - 1 - inset], radius=r - inset * 0.8, outline=hexrgb(ink), width=w)
    f = ImageFont.truetype(font, int(N * (0.56 if len(letters) == 1 else 0.46)))
    l, t, rr, b = g.textbbox((0, 0), letters, font=f)
    g.text(((N - (rr - l)) / 2 - l, (N - (b - t)) / 2 - t + N * 0.01), letters, font=f, fill=hexrgb(ink))
    img.resize((n, n), Image.LANCZOS).save(path, optimize=True)
    return True

TEMPLATE = """/* base-style: plastique{extra}
   {note}
   palette: {palette} */

QWidget {{ background-color: {bg}; color: {text}; selection-background-color: {sel}; selection-color: {seltext}; }}
QWidget:disabled {{ color: {disabled}; }}
QMainWindow, QDialog {{ background-color: {bg}; }}
QMainWindow::separator {{ background-color: {border}; width: 3px; height: 3px; }}
QMainWindow::separator:hover {{ background-color: {accent}; }}
QLabel, QCheckBox, QRadioButton {{ background-color: transparent; }}
QFrame[frameShape="4"], QFrame[frameShape="5"] {{ color: {border}; background-color: {border}; }}
QToolTip {{ background-color: {tip}; color: {tiptext}; border: 1px solid {border}; padding: 3px 5px; }}

QMenuBar {{ background-color: {panel}; border-bottom: 1px solid {border}; }}
QMenuBar::item {{ background: transparent; padding: 4px 9px; }}
QMenuBar::item:selected {{ background-color: {hover}; }}
QMenuBar::item:pressed {{ background-color: {sel}; color: {seltext}; }}
QMenu {{ background-color: {panel}; border: 1px solid {border}; padding: 3px 0px; }}
QMenu::item {{ background: transparent; padding: 4px 22px 4px 26px; }}
QMenu::item:selected {{ background-color: {sel}; color: {seltext}; }}
QMenu::item:disabled {{ color: {disabled}; }}
QMenu::separator {{ height: 1px; background-color: {border}; margin: 4px 8px; }}
QMenu::indicator {{ width: 13px; height: 13px; left: 6px; }}
QMenu::indicator:checked {{ image: url(images/{key}/check-dim.png); }}
QMenu::right-arrow {{ image: url(images/{key}/arrow-right.png); width: 10px; height: 10px; }}

QToolBar {{ background-color: {panel}; border: none; spacing: 2px; padding: 2px; }}
QToolBar::separator {{ background-color: {border}; width: 1px; height: 1px; margin: 4px; }}
QToolBar::handle {{ background-color: {border}; width: 3px; height: 3px; margin: 3px; }}
QToolButton {{ background-color: transparent; border: 1px solid transparent; border-radius: 3px; padding: 2px; }}
QToolButton:hover {{ background-color: {hover}; border-color: {border}; }}
QToolButton:pressed {{ background-color: {buttonpress}; }}
QToolButton:checked {{ background-color: {sel}; border-color: {accent}; color: {seltext}; }}
QToolButton::menu-indicator {{ image: url(images/{key}/arrow-down.png); width: 8px; height: 8px; subcontrol-position: bottom right; subcontrol-origin: padding; }}

QDockWidget {{ color: {text}; titlebar-close-icon: url(images/{key}/close.png); titlebar-normal-icon: url(images/{key}/float.png); }}
QDockWidget::title {{ background-color: {panel}; padding: 4px 6px; border-bottom: 1px solid {border}; text-align: left; }}
QDockWidget::close-button, QDockWidget::float-button {{ background: transparent; border: 1px solid transparent; border-radius: 2px; padding: 1px; }}
QDockWidget::close-button:hover, QDockWidget::float-button:hover {{ background-color: {hover}; border-color: {border}; }}
QTabWidget::pane {{ border: 1px solid {border}; top: -1px; background-color: {bg}; }}
QTabBar {{ background: transparent; }}
QTabBar::tab {{ background-color: {panel}; color: {dim}; border: 1px solid {border}; padding: 4px 10px; }}
QTabBar::tab:top {{ border-bottom: none; margin-right: 1px; border-top-left-radius: 3px; border-top-right-radius: 3px; }}
QTabBar::tab:bottom {{ border-top: none; margin-right: 1px; border-bottom-left-radius: 3px; border-bottom-right-radius: 3px; }}
QTabBar::tab:selected {{ background-color: {bg}; color: {text}; }}
QTabBar::tab:top:selected {{ border-top: 2px solid {accent}; }}
QTabBar::tab:bottom:selected {{ border-bottom: 2px solid {accent}; }}
QTabBar::tab:hover:!selected {{ background-color: {hover}; color: {text}; }}
QTabBar::close-button {{ image: url(images/{key}/close.png); subcontrol-position: right; }}
QTabBar::close-button:hover {{ image: url(images/{key}/close-hover.png); }}
QTabBar QToolButton {{ background-color: {panel}; border: 1px solid {border}; }}
QTabBar QToolButton::left-arrow {{ image: url(images/{key}/arrow-left.png); }}
QTabBar QToolButton::right-arrow {{ image: url(images/{key}/arrow-right.png); }}

QPushButton {{ background-color: {button}; color: {text}; border: 1px solid {border}; border-radius: 3px; padding: 3px 12px; }}
QPushButton:hover {{ background-color: {buttonhover}; border-color: {accent}; }}
QPushButton:pressed {{ background-color: {buttonpress}; }}
QPushButton:checked {{ background-color: {sel}; color: {seltext}; border-color: {accent}; }}
QPushButton:default {{ border-color: {accent}; }}
QPushButton:disabled {{ color: {disabled}; background-color: {bg}; border-color: {border}; }}
QPushButton::menu-indicator {{ image: url(images/{key}/arrow-down.png); width: 8px; height: 8px; subcontrol-position: right center; subcontrol-origin: padding; right: 4px; }}

QLineEdit, QTextEdit, QPlainTextEdit, QAbstractSpinBox {{ background-color: {base}; color: {text}; border: 1px solid {border}; border-radius: 2px; }}
QLineEdit {{ padding: 2px 4px; }}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QAbstractSpinBox:focus {{ border-color: {accent}; }}
QLineEdit:disabled, QTextEdit:disabled, QPlainTextEdit:disabled, QAbstractSpinBox:disabled {{ background-color: {bg}; color: {disabled}; }}
QAbstractSpinBox {{ padding: 1px 18px 1px 4px; }}
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {{ subcontrol-origin: border; width: 16px; background-color: {button}; border-left: 1px solid {border}; }}
QAbstractSpinBox::up-button {{ subcontrol-position: top right; border-top-right-radius: 2px; }}
QAbstractSpinBox::down-button {{ subcontrol-position: bottom right; border-bottom-right-radius: 2px; }}
QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover {{ background-color: {buttonhover}; }}
QAbstractSpinBox::up-button:pressed, QAbstractSpinBox::down-button:pressed {{ background-color: {buttonpress}; }}
QAbstractSpinBox::up-arrow {{ image: url(images/{key}/arrow-up.png); width: 8px; height: 8px; }}
QAbstractSpinBox::down-arrow {{ image: url(images/{key}/arrow-down.png); width: 8px; height: 8px; }}

QComboBox {{ background-color: {button}; color: {text}; border: 1px solid {border}; border-radius: 2px; padding: 2px 22px 2px 6px; }}
QComboBox:editable {{ background-color: {base}; }}
QComboBox:hover {{ border-color: {accent}; }}
QComboBox:on {{ border-color: {accent}; }}
QComboBox:disabled {{ color: {disabled}; background-color: {bg}; }}
QComboBox::drop-down {{ subcontrol-origin: padding; subcontrol-position: top right; width: 18px; border-left: 1px solid {border}; }}
QComboBox::down-arrow {{ image: url(images/{key}/arrow-down.png); width: 10px; height: 10px; }}
QComboBox QAbstractItemView {{ background-color: {base}; border: 1px solid {border}; selection-background-color: {sel}; selection-color: {seltext}; outline: 0; }}
QComboBox {{ combobox-popup: 0; }}
QComboBox#compareOP {{ padding: 2px 2px 2px 4px; }}
QComboBox#compareOP::drop-down {{ width: 14px; }}

QCheckBox, QRadioButton {{ spacing: 6px; }}
QCheckBox::indicator, QRadioButton::indicator, QAbstractItemView::indicator {{ width: 13px; height: 13px; background-color: {base}; border: 1px solid {handle}; }}
QCheckBox::indicator, QAbstractItemView::indicator {{ border-radius: 2px; }}
QRadioButton::indicator {{ border-radius: 7px; }}
QCheckBox::indicator:hover, QRadioButton::indicator:hover, QAbstractItemView::indicator:hover {{ border-color: {accent}; }}
QCheckBox::indicator:checked, QAbstractItemView::indicator:checked {{ background-color: {accent}; border-color: {accent}; image: url(images/{key}/check.png); }}
QCheckBox::indicator:indeterminate, QAbstractItemView::indicator:indeterminate {{ background-color: {accent}; border-color: {accent}; image: url(images/{key}/partial.png); }}
QRadioButton::indicator:checked {{ background-color: {accent}; border-color: {accent}; image: url(images/{key}/radio.png); }}
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled {{ background-color: {bg}; border-color: {border}; }}
QCheckBox::indicator:checked:disabled {{ background-color: {border}; }}

QGroupBox {{ border: 1px solid {border}; border-radius: 3px; margin-top: 14px; padding-top: 4px; }}
QGroupBox::title {{ subcontrol-origin: margin; subcontrol-position: top left; left: 8px; padding: 0px 4px; color: {dim}; }}

QAbstractItemView {{ background-color: {base}; alternate-background-color: {alt}; color: {text}; border: 1px solid {border}; outline: 0; }}
QAbstractItemView::item:hover {{ background-color: {hover}; }}
QAbstractItemView::item:selected {{ background-color: {sel}; color: {seltext}; }}
QAbstractItemView:disabled {{ color: {disabled}; }}
QTreeView::branch {{ background: transparent; }}
QTreeView::branch:has-children:!has-siblings:closed, QTreeView::branch:closed:has-children:has-siblings {{ border-image: none; image: url(images/{key}/arrow-right.png); }}
QTreeView::branch:open:has-children:!has-siblings, QTreeView::branch:open:has-children:has-siblings {{ border-image: none; image: url(images/{key}/arrow-down.png); }}
QHeaderView {{ background-color: {panel}; }}
QHeaderView::section {{ background-color: {panel}; color: {dim}; padding: 3px 6px; border: none; border-right: 1px solid {border}; border-bottom: 1px solid {border}; }}
QHeaderView::section:hover {{ background-color: {hover}; color: {text}; }}
QTableCornerButton::section {{ background-color: {panel}; border: none; border-right: 1px solid {border}; border-bottom: 1px solid {border}; }}

QScrollBar:vertical {{ background-color: {bg}; width: 12px; margin: 0px; }}
QScrollBar:horizontal {{ background-color: {bg}; height: 12px; margin: 0px; }}
QScrollBar::handle {{ background-color: {handle}; border-radius: 4px; }}
QScrollBar::handle:vertical {{ min-height: 24px; margin: 2px 2px; }}
QScrollBar::handle:horizontal {{ min-width: 24px; margin: 2px 2px; }}
QScrollBar::handle:hover {{ background-color: {handlehover}; }}
QScrollBar::add-line, QScrollBar::sub-line {{ width: 0px; height: 0px; border: none; background: none; }}
QScrollBar::add-page, QScrollBar::sub-page {{ background: none; }}
QAbstractScrollArea::corner {{ background-color: {bg}; }}

QSlider::groove:horizontal {{ height: 4px; background-color: {border}; border-radius: 2px; }}
QSlider::groove:vertical {{ width: 4px; background-color: {border}; border-radius: 2px; }}
QSlider::sub-page:horizontal {{ background-color: {accent}; border-radius: 2px; }}
QSlider::add-page:vertical {{ background-color: {accent}; border-radius: 2px; }}
QSlider::handle:horizontal {{ width: 12px; margin: -5px 0px; border-radius: 6px; background-color: {text}; }}
QSlider::handle:vertical {{ height: 12px; margin: 0px -5px; border-radius: 6px; background-color: {text}; }}
QSlider::handle:hover {{ background-color: {accent2}; }}
QSlider[styleSheet~="QSlider::groove:horizontal"]::sub-page:horizontal, QSlider[styleSheet~="QSlider::groove:horizontal"]::add-page:horizontal {{ background: transparent; }}
QProgressBar {{ background-color: {base}; border: 1px solid {border}; border-radius: 2px; text-align: center; color: {text}; }}
QProgressBar::chunk {{ background-color: {accent}; }}
QSplitter::handle {{ background-color: {bg}; }}
QSplitter::handle:hover {{ background-color: {accent}; }}
QStatusBar {{ background-color: {panel}; border-top: 1px solid {border}; color: {dim}; }}
QStatusBar::item {{ border: none; }}
QSizeGrip {{ background: transparent; }}
"""


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) + (255,)


def icon(path, size, draw, colour):
    """Draw at 8x and scale down."""
    s = 8
    img = Image.new("RGBA", (size * s, size * s), (0, 0, 0, 0))
    draw(ImageDraw.Draw(img), size * s, hexrgb(colour), s)
    img.resize((size, size), Image.LANCZOS).save(path)


def chevron(direction):
    def d(g, n, c, s):
        m = n * 0.26
        pts = {"down": [(m, n * 0.36), (n / 2, n - n * 0.34), (n - m, n * 0.36)],
               "up": [(m, n - n * 0.36), (n / 2, n * 0.34), (n - m, n - n * 0.36)],
               "right": [(n * 0.36, m), (n - n * 0.34, n / 2), (n * 0.36, n - m)],
               "left": [(n - n * 0.36, m), (n * 0.34, n / 2), (n - n * 0.36, n - m)]}[direction]
        g.line(pts, fill=c, width=int(1.7 * s), joint="curve")
    return d


def triangle(direction):
    """VGUI's filled arrows (arrows=triangle)"""
    def d(g, n, c, s):
        a, b, m = n * 0.3, n * 0.7, n * 0.12
        pts = {"down": [(m, a), (n - m, a), (n / 2, b)], "up": [(m, b), (n - m, b), (n / 2, a)],
               "right": [(a, m), (a, n - m), (b, n / 2)], "left": [(b, m), (b, n - m), (a, n / 2)]}[direction]
        g.polygon(pts, fill=c)
    return d


def check(g, n, c, s):
    g.line([(n * 0.2, n * 0.52), (n * 0.42, n * 0.74), (n * 0.8, n * 0.28)], fill=c, width=int(1.9 * s), joint="curve")


def partial(g, n, c, s):
    g.line([(n * 0.24, n / 2), (n * 0.76, n / 2)], fill=c, width=int(1.9 * s))


def dot(g, n, c, s):
    r = n * 0.2
    g.ellipse([n / 2 - r, n / 2 - r, n / 2 + r, n / 2 + r], fill=c)


def cross(g, n, c, s):
    m = n * 0.22
    g.line([(m, m), (n - m, n - m)], fill=c, width=int(1.5 * s))
    g.line([(m, n - m), (n - m, m)], fill=c, width=int(1.5 * s))


def float_icon(g, n, c, s):
    w = int(1.2 * s)
    g.rectangle([n * 0.34, n * 0.14, n * 0.86, n * 0.62], outline=c, width=w)
    g.rectangle([n * 0.14, n * 0.38, n * 0.62, n * 0.86], outline=c, width=w)


# ---- view color presets: themes/<Name>.colors ----
# Radiant's view colors are 48 rows of r, g, b, a (registry Treyarch\Radiant\Colors). A preset may list only some keys.

def rgbf(h):
    return tuple(c / 255 for c in hexrgb(h)[:3])


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def view_colors(p):
    base, bg, panel, text, dim = (rgbf(p[k]) for k in ("base", "bg", "panel", "text", "dim"))
    accent, accent2, sel, seltext = (rgbf(p[k]) for k in ("accent", "accent2", "sel", "seltext"))
    dark = lum(bg) < 0.5
    c = {}
    if dark:
        c["COLOR_GRIDBACK"] = base
        c["COLOR_GRIDMINOR"] = mix(base, text, 0.07)
        c["COLOR_GRIDMAJOR"] = mix(base, text, 0.16)
        c["COLOR_GRIDBLOCK"] = mix(base, accent, 0.55)
        c["COLOR_CAMERABACK"] = mix(base, bg, 0.5)
        c["COLOR_BRUSHES"] = mix(text, accent2, 0.45)
        c["COLOR_LIGHT"] = (0.95, 0.82, 0.35)
        c["COLOR_WIREFRAME"] = mix(dim, text, 0.25)
        c["COLOR_PREFAB_WIREFRAME"] = (1.0, 0.36, 0.36)
    else:
        c["COLOR_GRIDBACK"] = base
        c["COLOR_GRIDMINOR"] = mix(base, text, 0.09)
        c["COLOR_GRIDMAJOR"] = mix(base, text, 0.2)
        c["COLOR_GRIDBLOCK"] = mix(base, accent, 0.5)
        c["COLOR_CAMERABACK"] = mix(bg, text, 0.32)
        c["COLOR_BRUSHES"] = mix(text, accent, 0.4)
        c["COLOR_LIGHT"] = (0.0, 0.0, 0.0)
        c["COLOR_WIREFRAME"] = dim
        c["COLOR_PREFAB_WIREFRAME"] = (0.85, 0.0, 0.0)
    c["COLOR_GRIDTEXT"] = dim
    c["COLOR_VIEWNAME"] = accent2 if dark else accent
    c["COLOR_SIZEINFO"] = accent2 if dark else accent
    c["COLOR_XYWND_CAMERA_ICON"] = accent2 if dark else accent
    c["COLOR_TEXTUREBACK"] = panel
    c["COLOR_TEXTURE_IMAGEBACK"] = mix(panel, text, 0.12)
    c["COLOR_TEXTURE_IMAGEBACK_INUSE"] = mix(panel, accent, 0.35)
    c["COLOR_LYRMTL_SELECTEDBACK"] = sel
    c["COLOR_LYRMTL_SELECTEDTEXT"] = seltext
    # Grid View: selected brushes in a hue away from the accent
    hue = colorsys.rgb_to_hls(*accent)[0] * 360
    c["COLOR_SELBRUSHES"] = shade(hexf(0xff7a3d if 70 <= hue <= 170 else 0x42ffa3), 0.65 if dark else 0.36, 0.9, accent, 0)
    c["COLOR_SCRIPT_BRUSHMODEL"] = shade(hexf(0x0080cc), OBJ_L[dark], 0.8, accent, OBJ_TINT)
    # Camera View
    cam = c["COLOR_CAMERABACK"]
    c["COLOR_CAMWND_GRID"] = mix(cam, text, 0.3)
    c["COLOR_CAMWND_GRID_SUBDIVISION"] = mix(cam, text, 0.14)
    c["COLOR_CAMWND_GRID_TEXT"] = text
    c["COLOR_CAMWND_GRID_AXIS"] = accent2 if dark else accent
    for key, rgb in (("X", 0xff0000), ("Y", 0x00ff00), ("Z", 0x0000ff)):
        c[f"COLOR_{key}AXIS_MANIP_HANDLE"] = shade(hexf(rgb), 0.62 if dark else 0.42, 0.9, accent, 0)
    # Shared: each object keeps its hue at a lightness that reads on the theme, leaning to the accent
    for key, (rgb, sat) in SHARED_HUES.items():
        c[key] = shade(hexf(rgb), OBJ_L[dark], sat, accent, OBJ_TINT)
    c["COLOR_MODEL"] = shade(hexf(0xe0e3ff), 0.8 if dark else 0.3, 0.35, accent, OBJ_TINT)   # Radiant's pale blue models
    c["COLOR_FUNCGROUP"] = mix(base, text, 0.55)
    c["COLOR_CLIPPER"] = accent2 if dark else accent
    for key in ("COLOR_SELBRUSHESCAMERA", "COLOR_FACESELECTION", "COLOR_SELTEXTURETINT"):
        c[key] = accent
    return c


# the Shared tab's object colours: Radiant's default hue and a saturation each, shade() sets the lightness
SHARED_HUES = {"COLOR_DETAIL": (0xa8abff, 0.45), "COLOR_PORTAL": (0x1a66ff, 0.85), "COLOR_FUNCCULLGROUP": (0x00a0a0, 0.7),
               "COLOR_WEAPONCLIP": (0xc9cc1a, 0.75), "COLOR_SCRIPTMODEL": (0x0080cc, 0.8), "COLOR_DYNMODEL": (0xd608d6, 0.75),
               "COLOR_NONCOLLIDING": (0xcc9900, 0.85), "COLOR_FROZEN": (0xbf0000, 0.8), "COLOR_TERRAINPATCH": (0x33ff33, 0.7),
               "COLOR_CURVEPATCH": (0x33ff33, 0.7)}
OBJ_L = {True: 0.68, False: 0.36}   # lightness on dark / light views
OBJ_TINT = 0.12                     # lean toward the accent


def hexf(v):
    return ((v >> 16 & 255) / 255, (v >> 8 & 255) / 255, (v & 255) / 255)


def shade(rgb, lightness, saturation, accent, tint):
    """rgb's hue at the given lightness and saturation, mixed toward accent by tint"""
    h = colorsys.rgb_to_hls(*rgb)[0]
    return mix(colorsys.hls_to_rgb(h, lightness, saturation), accent, tint)


# the colour schemes of id's Q3Radiant (Misc > Colors > Themes), only the keys those set
CLASSIC = {
    "QE4 Original": {"COLOR_TEXTUREBACK": (.25, .25, .25), "COLOR_GRIDBACK": (1, 1, 1), "COLOR_GRIDMINOR": (.75, .75, .75),
                     "COLOR_GRIDMAJOR": (.5, .5, .5), "COLOR_CAMERABACK": (.25, .25, .25), "COLOR_GRIDBLOCK": (0, 0, 1),
                     "COLOR_GRIDTEXT": (0, 0, 0), "COLOR_SELBRUSHES": (1, 0, 0), "COLOR_CLIPPER": (0, 0, 1),
                     "COLOR_BRUSHES": (0, 0, 0), "COLOR_VIEWNAME": (.5, 0, .75)},
    "Q3Radiant": {"COLOR_TEXTUREBACK": (.25, .25, .25), "COLOR_GRIDBACK": (1, 1, 1), "COLOR_GRIDMINOR": (1, 1, 1),
                  "COLOR_GRIDMAJOR": (.5, .5, .5), "COLOR_CAMERABACK": (.25, .25, .25), "COLOR_GRIDBLOCK": (0, 0, 1),
                  "COLOR_GRIDTEXT": (0, 0, 0), "COLOR_SELBRUSHES": (1, 0, 0), "COLOR_CLIPPER": (0, 0, 1),
                  "COLOR_BRUSHES": (0, 0, 0), "COLOR_VIEWNAME": (.5, 0, .75)},
    "Black and Green": {"COLOR_TEXTUREBACK": (.25, .25, .25), "COLOR_GRIDBACK": (0, 0, 0), "COLOR_GRIDMINOR": (0, 0, 0),
                        "COLOR_CAMERABACK": (.25, .25, .25), "COLOR_GRIDMAJOR": (.3, .5, .5), "COLOR_GRIDBLOCK": (0, 0, 1),
                        "COLOR_GRIDTEXT": (1, 1, 1), "COLOR_SELBRUSHES": (1, 0, 0), "COLOR_CLIPPER": (0, 0, 1),
                        "COLOR_BRUSHES": (1, 1, 1), "COLOR_VIEWNAME": (.7, .7, 0)},
}


# the Classic theme's views: Q3Radiant's scheme with Radiant's green selection (red means frozen and prefab wireframes)
CLASSIC_THEME = {k: v for k, v in CLASSIC["Q3Radiant"].items() if k != "COLOR_SELBRUSHES"}


def write_colors(name, colors, note):
    with open(os.path.join(ROOT, f"{name}.colors"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("; MTPlugins view colors\n")
        for key, v in colors.items():
            f.write(key + "=" + ", ".join("%g" % round(x, 3) for x in v) + "\n")


# every palette key, in the order of the "palette:" header line (the Theme editor reads it back)
PALETTE_KEYS = ["bg", "panel", "base", "alt", "border", "text", "dim", "disabled", "accent", "accent2", "sel", "seltext",
                "hover", "button", "buttonhover", "buttonpress", "tip", "tiptext", "handle", "handlehover", "mark"]
NOTE = "Generated by tools/make_themes.py"


def write_pack(name, picture):
    """themes/icons/<name>/: the picture's middle square at the size of every icon in tools/editor_icons.txt"""
    tools = os.path.dirname(os.path.abspath(__file__))
    src = Image.open(os.path.join(tools, picture)).convert("RGBA")
    side = min(src.size)
    left, top = (src.width - side) // 2 + side // 20, (src.height - side) // 2   # face sits right of centre
    left = min(left, src.width - side)
    square = src.crop((left, top, left + side, top + side))
    out = os.path.join(ROOT, "icons", name)
    n = 0
    with open(os.path.join(tools, "editor_icons.txt"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            rel, w, h = line.rstrip("\n").split("\t")
            w, h = int(w), int(h)
            s = max(w, h)
            img = square.resize((s, s), Image.LANCZOS).crop(((s - w) // 2, (s - h) // 2, (s - w) // 2 + w, (s - h) // 2 + h))
            dst = os.path.join(out, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            img.convert("RGB").save(dst) if dst.lower().endswith(".bmp") else img.save(dst, optimize=True)
            n += 1
    return n


def write_template():
    """themes/template/theme.qss.in: the sheet with @key@ placeholders, for the Theme editor"""
    os.makedirs(os.path.join(ROOT, "template"), exist_ok=True)
    fields = {k: f"@{k}@" for k in PALETTE_KEYS}
    text = TEMPLATE.format(name="@name@", key="@key@", extra="@extra@", note="@note@", palette="@palette@", **fields)
    with open(os.path.join(ROOT, "template", "theme.qss.in"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)


def main():
    os.makedirs(ROOT, exist_ok=True)
    write_template()
    for name, p in PALETTES.items():
        key = name.lower().replace(" ", "")
        img = os.path.join(ROOT, "images", key)
        os.makedirs(img, exist_ok=True)
        arrow = triangle if p.get("arrows") == "triangle" else chevron
        for d in ("up", "down", "left", "right"):
            icon(os.path.join(img, f"arrow-{d}.png"), 10, arrow(d), p["dim"])
        icon(os.path.join(img, "check.png"), 13, check, p["mark"])
        icon(os.path.join(img, "check-dim.png"), 13, check, p["text"])
        icon(os.path.join(img, "partial.png"), 13, partial, p["mark"])
        icon(os.path.join(img, "radio.png"), 13, dot, p["mark"])
        icon(os.path.join(img, "close.png"), 10, cross, p["dim"])
        icon(os.path.join(img, "close-hover.png"), 10, cross, p["text"])
        icon(os.path.join(img, "float.png"), 10, float_icon, p["dim"])
        with open(os.path.join(ROOT, f"{name}.qss"), "w", encoding="utf-8", newline="\r\n") as f:
            recolor = f"{p['icons']} {p['bg']}"
            extra = f"\n   icon-recolor: {recolor}"
            if p.get("own_icons"):   # keep the editors' own icons
                extra = ""
            logo_lines = ""
            for app, (letters, fill, ink) in p.get("logos", {}).items():
                file = f"logo-{app}.png" if app else "logo.png"
                if write_logo(os.path.join(img, file), letters, fill, ink):
                    logo_lines += f"\n   logo{'-' + app if app else ''}: images/{key}/{file}"
                button = os.path.join(ROOT, "icons", name, *LOGO_BUTTONS[app].split("/"))
                os.makedirs(os.path.dirname(button), exist_ok=True)
                write_logo(button, letters, fill, ink, 32)
            if logo_lines:
                logo_lines += f"\n   icon-pack: icons/{name}"
                extra = logo_lines + extra
            if "pack" in p:   # one picture for every icon
                n = write_pack(name, p["pack"])
                extra = f"\n   icon-pack: icons/{name}"
            palette = " ".join(f"{k}={p[k]}" for k in PALETTE_KEYS) + f" icons={p['icons']}"
            fields = {k: v for k, v in p.items() if k not in ("icons", "pack", "lite", "arrows", "css", "logos", "own_icons", "colors")}
            f.write(TEMPLATE.format(name=name, key=key, extra=extra, note=NOTE, palette=palette, **fields))
            if "css" in p:
                f.write(EXTRA_CSS[p["css"]].format(key=key, **{k: v for k, v in p.items() if k != "css"}))
        if p.get("colors", True): write_colors(name, view_colors(p), f"the views to go with the {name} theme")
        elif os.path.exists(os.path.join(ROOT, f"{name}.colors")): os.remove(os.path.join(ROOT, f"{name}.colors"))   # Radiant's defaults
    for name, colors in CLASSIC.items():
        write_colors(name, colors, "the colour scheme of id's Radiant of that name")
    write_colors("Classic", CLASSIC_THEME, "the views to go with the Classic theme: Q3Radiant's scheme, Radiant's green selection")
    print("themes:", ", ".join(PALETTES), "+ view colors", ", ".join(CLASSIC), "->", os.path.normpath(ROOT))


if __name__ == "__main__":
    main()
