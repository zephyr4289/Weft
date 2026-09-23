/**
 * Weft Studio — Modern Darcula / Deep Slate design tokens (Android Studio
 * aesthetic). Single source of truth for every panel.
 */

export const STUDIO_THEME = {
  // chrome
  bg: '#1E1F22',           // editor background
  chrome: '#2B2D30',       // tool window headers / bars
  chromeAlt: '#313438',    // panel bodies
  chromeDeep: '#26282B',   // menu bar
  border: '#393B40',
  borderSoft: '#2F3134',
  selection: '#2E436E',
  hover: '#35373B',
  // semantic
  text: '#BCBEC4',
  textBright: '#DFE1E5',
  textDim: '#6F737A',
  accent: '#3574F0',       // accent blue
  accentSoft: '#2B4B7C',
  green: '#499C54',
  greenBright: '#5FAD6B',
  red: '#E06C75',
  redBright: '#F27178',
  yellow: '#DCA878',
  orange: '#CC7832',
  purple: '#C77DBB',
  cyan: '#4A88C7',
  // editor tokens (Darcula)
  tokText: '#A9B7C6',
  tokKeyword: '#CC7832',
  tokType: '#A9B7C6',
  tokPrim: '#6FAFBD',
  tokString: '#6A8759',
  tokNumber: '#6897BB',
  tokComment: '#808080',
  tokAttr: '#B3AE60',
  tokField: '#9876AA',
  tokPunct: '#A9B7C6',
} as const;

export type StudioTheme = typeof STUDIO_THEME;

/** Minimal inline-style record (keeps theme dependency-free of React types). */
export interface CSSProps {
  [key: string]: string | number | undefined;
}
