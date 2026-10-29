# Spell Checker

A plugin bundled with the GTK port, in the spirit of
[DSpellCheck](https://github.com/Predelnik/DSpellCheck) (which the Windows
installer bundles): it checks spelling as you type and underlines misspelled
words with a red squiggle. It is not a port of DSpellCheck's code. It is
written for this port on the plugin SDK and GTK, and gets its dictionaries
from [Enchant](https://rrthomas.github.io/enchant/), which uses the system's
Hunspell, Nuspell or Aspell dictionaries.

Source: `linux/plugins/SpellCheck`. It is installed with the editor as
`<libdir>/notepad-plus-plus-gtk/plugins/SpellCheck/SpellCheck.so` and shows
up under **Plugins ▸ Spell Checker**.

## Dictionaries

Enchant (`libenchant-2-2`) is loaded when the plugin starts. Without it the
plugin still loads, and its commands say what to install. Install a
dictionary for each language you write in:

```sh
sudo apt install libenchant-2-2 hunspell-en-us hunspell-de-de   # or aspell-fr, ...
```

The default language is the first of your desktop's languages (`LANGUAGE`,
`LANG`) that has a dictionary, else English. **Settings…** lists the
installed dictionaries. Tick several to accept a word that any one of them
knows, for example when you write in two languages.

## Using it

- **Check Spelling as You Type** (on by default) underlines the misspelled
  words on screen. The word you are typing is marked when the caret leaves
  it, not while it is half typed.
- **Right-click** an underlined word for its suggestions (a click replaces
  the word), **Ignore "word"** (until Notepad++ quits) and **Add "word" to
  Dictionary**. Scintilla's usual items (Undo … Select All) follow. A
  right-click anywhere else opens the usual menu.
- **Suggestions…** opens the same menu below the word at the caret. Give it
  a key in the Shortcut Mapper (Plugin commands) to use it from the
  keyboard.
- **Next Misspelling** and **Previous Misspelling** select the next or
  previous misspelled word in the whole document, wrapping round its end.

## What is checked

In text documents, all of it: Normal Text, and files named `.txt`, `.md`,
`.rst`, `.adoc`, `.org`, `.tex` and the like. Markdown code spans, code
blocks and links are skipped, and so are LaTeX commands.

In source code, only comments and strings. For most languages (C, C++, C#,
Java, JavaScript, Python, PHP, HTML, XML, shell, Perl, Ruby, Lua, Rust,
Pascal, batch, makefiles and others) the lexer says which of its styles
those are. For HTML and XML that is the page text and comments, not the
tags or attribute values. SQL, CSS, YAML, JSON, INI, PowerShell, R,
Fortran, Lisp and MATLAB have their styles listed in the plugin. For User
Defined Languages it is the comments and delimiters. Languages that fit none
of these are not checked unless you turn off **In source code, check only
comments and strings**.

These words are always left out:

- words with `_`, and single letters;
- URLs, e-mail addresses, paths (`/usr/share`, `C:\Users`) and dotted
  names (`config.yaml`);
- calls and qualified names (`name()`, `std::vector`), variables and
  format codes (`$path`, `%s`), escapes (`\n`) and entities (`&amp;`).

The settings can also leave out words with digits (`utf8`), words in
CAPITALS, and words with capitals inside (`camelCase`, `iPhone`). All three
are left out by default.

## Files

In the plugins' configuration folder
(`~/.config/notepad-plus-plus-gtk/plugins/config`):

- `SpellCheck.ini` holds the settings.
- `SpellCheck.dic` is your personal dictionary, one word per line, shared
  by all languages. **Settings ▸ Edit Personal Dictionary** opens it in
  Notepad++. When you save it there, the plugin reads it again.

## How it works

- **Underlines.** The plugin takes an indicator with
  `NPPM_ALLOCATEINDICATOR`. A short timer after each edit, scroll or tab
  change checks the lines on screen of both views: it lexes them
  (`SCI_COLOURISE`), reads text and styles together
  (`SCI_GETSTYLEDTEXTFULL`), finds the words (`SpellText.cpp`) and asks
  Enchant, with the results cached.
- **Which styles.** It uses the lexer's style tags (`SCI_GETNAMEDSTYLES`,
  `SCI_TAGSOFSTYLE`: "comment", "literal string") where the lexer has them,
  and a short table of styles for the others.
- **Right-click menu.** A `button-press-event` handler on each editor
  widget (`NPPM_GTKGETSCINTILLAWIDGET`) runs before Scintilla's own. On an
  underlined word it shows the plugin's menu and stops the event; anywhere
  else it lets Scintilla show its own menu.
- **Loading Enchant.** It is opened with `dlopen` and never unloaded,
  because it registers GObject types. Building the plugin therefore needs
  no Enchant headers.

## Tests

- `spell_check` (`tests/spellcheck_test.cpp`, no display) covers:
  - word finding: apostrophes, byte offsets in other scripts, and each rule
    above;
  - the choice of styles for C++, HTML, plain text, Markdown, UDL, SQL and
    unknown lexers;
  - when Enchant and `en_US` are installed: checking, suggestions, Ignore,
    and the personal dictionary (adding, editing outside, reloading).
- `spell_check_editor` (`tests/spellcheck_panel_test.cpp`) runs the plugin
  in a live window:
  - underlines in a text file, and in a C++ file only in its comment and
    string;
  - a right-click suggestion replaces the word, and a right-click on a
    correct word goes on to Scintilla;
  - Add to Dictionary writes `SpellCheck.dic`;
  - a typed word is marked only once the caret leaves it;
  - Next and Previous Misspelling, including the wrap round the end;
  - turning checking off clears the underlines.

  Without Enchant or its `en_US` dictionary it reports SKIP and passes.
