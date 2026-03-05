"use client";

/**
 * SqlEditor — CodeMirror 6 wrapper with SixSevenDB SQL syntax highlighting,
 * autocomplete, and keyboard shortcuts.
 */

import { useRef, useEffect, useCallback } from "react";
import { EditorView, keymap, placeholder as cmPlaceholder } from "@codemirror/view";
import { EditorState } from "@codemirror/state";
import { defaultKeymap, indentWithTab, history, historyKeymap } from "@codemirror/commands";
import { oneDark } from "@codemirror/theme-one-dark";
import { autocompletion } from "@codemirror/autocomplete";
import { searchKeymap, highlightSelectionMatches } from "@codemirror/search";
import { syntaxHighlighting, defaultHighlightStyle, bracketMatching, indentOnInput } from "@codemirror/language";
import {
  sixsevenSQL,
  sixsevenCompletionSource,
  type SchemaCompletionData,
} from "@/lib/sixseven-sql-lang";

interface SqlEditorProps {
  /** Initial SQL content. */
  value?: string;
  /** Called whenever the editor content changes. */
  onChange?: (value: string) => void;
  /** Called when Ctrl+Enter is pressed. */
  onExecute?: (sql: string) => void;
  /** Called when Ctrl+S is pressed. */
  onSave?: (sql: string) => void;
  /** Called when Ctrl+L is pressed. */
  onClearResults?: () => void;
  /** Schema data for autocomplete. */
  schemaData?: SchemaCompletionData;
  /** Placeholder text. */
  placeholder?: string;
}

export function SqlEditor({
  value = "",
  onChange,
  onExecute,
  onSave,
  onClearResults,
  schemaData,
  placeholder = "Enter SQL query... (Ctrl+Enter to execute)",
}: SqlEditorProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const viewRef = useRef<EditorView | null>(null);
  const callbacksRef = useRef({ onChange, onExecute, onSave, onClearResults });

  // Keep callbacks ref up to date without recreating editor
  useEffect(() => {
    callbacksRef.current = { onChange, onExecute, onSave, onClearResults };
  }, [onChange, onExecute, onSave, onClearResults]);

  // Build custom keybindings
  const getCustomKeymap = useCallback(() => {
    return keymap.of([
      {
        key: "Ctrl-Enter",
        mac: "Cmd-Enter",
        run: (view) => {
          const sql = view.state.doc.toString().trim();
          if (sql) callbacksRef.current.onExecute?.(sql);
          return true;
        },
      },
      {
        key: "Ctrl-s",
        mac: "Cmd-s",
        run: (view) => {
          const sql = view.state.doc.toString().trim();
          if (sql) callbacksRef.current.onSave?.(sql);
          return true;
        },
        preventDefault: true,
      },
      {
        key: "Ctrl-l",
        mac: "Cmd-l",
        run: () => {
          callbacksRef.current.onClearResults?.();
          return true;
        },
      },
    ]);
  }, []);

  // Create editor on mount
  useEffect(() => {
    if (!containerRef.current) return;

    const completionSource = schemaData
      ? sixsevenCompletionSource(schemaData)
      : undefined;

    const extensions = [
      // Custom keybindings first (highest priority)
      getCustomKeymap(),
      // Tab indent/outdent
      keymap.of([indentWithTab]),
      // History (undo/redo)
      history(),
      keymap.of([...historyKeymap, ...defaultKeymap, ...searchKeymap]),
      // SixSevenDB SQL language
      sixsevenSQL(),
      // Syntax highlighting
      syntaxHighlighting(defaultHighlightStyle, { fallback: true }),
      // Bracket matching
      bracketMatching(),
      // Auto-indent
      indentOnInput(),
      // Selection matches
      highlightSelectionMatches(),
      // Dark theme
      oneDark,
      // Autocomplete
      autocompletion({
        override: completionSource ? [completionSource] : undefined,
        activateOnTyping: true,
        maxRenderedOptions: 30,
      }),
      // Placeholder
      cmPlaceholder(placeholder),
      // Update listener for onChange
      EditorView.updateListener.of((update) => {
        if (update.docChanged) {
          callbacksRef.current.onChange?.(update.state.doc.toString());
        }
      }),
      // Styling: fill container, line wrapping
      EditorView.theme({
        "&": {
          height: "100%",
          fontSize: "13px",
        },
        ".cm-scroller": {
          overflow: "auto",
          fontFamily: "'JetBrains Mono', 'Fira Code', 'Consolas', monospace",
        },
        ".cm-content": {
          caretColor: "#528bff",
          padding: "8px 0",
        },
        ".cm-gutters": {
          backgroundColor: "#1a1b26",
          borderRight: "1px solid #2d2d3d",
          color: "#4a4a5a",
        },
        ".cm-activeLineGutter": {
          backgroundColor: "#1e1f2e",
        },
        ".cm-activeLine": {
          backgroundColor: "#1e1f2e80",
        },
        ".cm-tooltip.cm-tooltip-autocomplete": {
          backgroundColor: "#1a1b26",
          border: "1px solid #2d2d3d",
        },
        ".cm-tooltip.cm-tooltip-autocomplete > ul > li": {
          padding: "4px 8px",
        },
        ".cm-tooltip.cm-tooltip-autocomplete > ul > li[aria-selected]": {
          backgroundColor: "#2d2d3d",
        },
      }),
      EditorView.lineWrapping,
    ];

    const state = EditorState.create({
      doc: value,
      extensions,
    });

    const view = new EditorView({
      state,
      parent: containerRef.current,
    });

    viewRef.current = view;

    return () => {
      view.destroy();
      viewRef.current = null;
    };
    // Only re-create editor when schemaData changes
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [schemaData]);

  // Sync external value changes (e.g., loading from history)
  useEffect(() => {
    const view = viewRef.current;
    if (!view) return;
    const currentDoc = view.state.doc.toString();
    if (currentDoc !== value) {
      view.dispatch({
        changes: {
          from: 0,
          to: currentDoc.length,
          insert: value,
        },
      });
    }
  }, [value]);

  return (
    <div
      ref={containerRef}
      className="h-full w-full overflow-hidden border border-gray-800 rounded"
    />
  );
}
