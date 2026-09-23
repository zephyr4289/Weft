// Minimal markdown -> HTML for site docs (headings, lists, tables, code, bold/italic/code, links).
export function md(src) {
  const lines = src.replace(/\r/g, '').split('\n');
  let html = '', i = 0, inCode = false, codeBuf = [], listType = null, para = [];
  const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const inline = (s) => esc(s)
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
    .replace(/(^|\W)\*([^*\s][^*]*)\*/g, '$1<em>$2</em>')
    .replace(/\[([^\]]+)\]\(([^)]+)\)/g, (m, t, u) => {
      if (u.startsWith('http') || u.startsWith('#')) return `<a href="${u}">${t}</a>`;
      return `<a href="${u.replace(/\.md$/, '').replace(/^docs\//, '/Weft/docs/')}">${t}</a>`;
    });
  const flushPara = () => { if (para.length) { html += `<p>${inline(para.join(' '))}</p>\n`; para = []; } };
  const closeList = () => { if (listType) { html += `</${listType}>\n`; listType = null; } };
  while (i < lines.length) {
    const l = lines[i];
    if (l.trim().startsWith('```')) {
      if (inCode) { html += `<pre class="doc-pre"><code>${esc(codeBuf.join('\n'))}</code></pre>\n`; codeBuf = []; inCode = false; }
      else { flushPara(); closeList(); inCode = true; }
      i++; continue;
    }
    if (inCode) { codeBuf.push(l); i++; continue; }
    if (/^\s*$/.test(l)) { flushPara(); closeList(); i++; continue; }
    const h = l.match(/^(#{1,4})\s+(.*)/);
    if (h) { flushPara(); closeList(); const n = h[1].length; const id = h[2].toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, ''); html += `<h${n} id="${id}">${inline(h[2])}</h${n}>\n`; i++; continue; }
    if (/^(-{3,}|\*{3,})\s*$/.test(l)) { flushPara(); closeList(); html += '<hr/>\n'; i++; continue; }
    if (/^>\s?/.test(l)) { flushPara(); closeList(); const buf = []; while (i < lines.length && /^>\s?/.test(lines[i])) { buf.push(lines[i].replace(/^>\s?/, '')); i++; } html += `<blockquote>${inline(buf.join(' '))}</blockquote>\n`; continue; }
    if (/^\|/.test(l)) {
      flushPara(); closeList();
      const rows = []; while (i < lines.length && /^\|/.test(lines[i])) { rows.push(lines[i]); i++; }
      const cells = (r) => r.split('|').slice(1, -1).map(c => c.trim());
      let t = '<table class="doc-table"><thead><tr>' + cells(rows[0]).map(c => `<th>${inline(c)}</th>`).join('') + '</tr></thead><tbody>';
      const body = rows.slice(/^\s*\|[\s:|-]+\|\s*$/.test(rows[1] ?? '') ? 2 : 1);
      for (const r of body) t += '<tr>' + cells(r).map(c => `<td>${inline(c)}</td>`).join('') + '</tr>';
      html += t + '</tbody></table>\n'; continue;
    }
    const ul = l.match(/^[-*]\s+(.*)/), ol = l.match(/^\d+\.\s+(.*)/);
    if (ul || ol) {
      flushPara();
      const want = ul ? 'ul' : 'ol';
      if (listType !== want) { closeList(); html += `<${want}>\n`; listType = want; }
      html += `<li>${inline((ul ?? ol)[1])}</li>\n`; i++; continue;
    }
    para.push(l.trim()); i++;
  }
  flushPara(); closeList();
  if (inCode) html += `<pre class="doc-pre"><code>${esc(codeBuf.join('\n'))}</code></pre>\n`;
  return html;
}
