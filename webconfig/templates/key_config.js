/* Key-name and bounded text parsing for the config page (#23).
 * Keep the names and capacities aligned with src/core/dh_config_text.h. */
const keyConfig = (() => {
  const names = new Map();
  for (let i = 0; i < 26; ++i) names.set(String.fromCharCode(97 + i), 0x04 + i);
  for (let i = 1; i <= 9; ++i) names.set(String(i), 0x1d + i);
  names.set('0', 0x27);
  for (let i = 1; i <= 12; ++i) names.set(`f${i}`, 0x39 + i);

{% for name, usage in key_names %}
  names.set('{{ name }}', {{ usage }});
{% endfor %}

  const fail = (line, token, message) => ({ ok: false, line, token, message });
  const usage = (raw, line) => {
    const name = raw.trim().toLowerCase();
    return names.has(name)
      ? { ok: true, value: names.get(name) }
      : fail(line, raw.trim(), raw.trim() ? `unknown key “${raw.trim()}”` : 'expected a key');
  };

  function overrides(text, capacity = 32) {
    const output = [];
    const lines = text.split(/\r?\n/);
    for (let i = 0; i < lines.length; ++i) {
      const line = lines[i].trim();
      if (!line) continue;
      const parts = line.split('=');
      if (parts.length !== 2)
        return fail(i + 1, line, 'expected one “=” between key names');
      const from = usage(parts[0], i + 1);
      if (!from.ok) return from;
      const to = usage(parts[1], i + 1);
      if (!to.ok) return to;
      if (output.length === capacity)
        return fail(i + 1, parts[0].trim(), `more than ${capacity} overrides`);
      output.push({ from: from.value, to: to.value });
    }
    return { ok: true, value: output };
  }

  function keys(text, capacity = 16) {
    const output = [];
    const lines = text.split(/\r?\n/);
    for (let i = 0; i < lines.length; ++i) {
      if (!lines[i].trim()) continue;
      const parts = lines[i].split(',');
      for (const part of parts) {
        const key = usage(part, i + 1);
        if (!key.ok) return key;
        if (output.length === capacity)
          return fail(i + 1, part.trim(), `more than ${capacity} passthrough keys`);
        output.push(key.value);
      }
    }
    return { ok: true, value: output };
  }

  function chord(text, capacity = 6) {
    const output = [];
    for (const part of text.split('+')) {
      const key = usage(part, 1);
      if (!key.ok) return key;
      if (output.length === capacity)
        return fail(1, part.trim(), `more than ${capacity} keys in the chord`);
      output.push(key.value);
    }
    return { ok: true, value: output };
  }

  /* Text fields supplied by #26/#27 call this on input. A failed parse leaves
     their last parsed value untouched and puts the useful error beside the
     field, where Save can see aria-invalid instead of discarding a line. */
  function validateField(element, kind) {
    const result = ({ overrides, keys, chord })[kind](element.value);
    let error = element.nextElementSibling;
    if (!error || !error.classList.contains('config-text-error')) {
      error = document.createElement('div');
      error.className = 'config-text-error';
      error.setAttribute('role', 'alert');
      element.insertAdjacentElement('afterend', error);
    }
    element.setAttribute('aria-invalid', result.ok ? 'false' : 'true');
    error.textContent = result.ok ? '' : `Line ${result.line}: ${result.message}`;
    if (result.ok) element.parsedConfigValue = result.value;
    return result.ok;
  }

  function validateAll() {
    let valid = true;
    document.querySelectorAll('[data-key-config]').forEach(element => {
      if (!validateField(element, element.getAttribute('data-key-config'))) valid = false;
    });
    return valid;
  }

  return { usage, overrides, keys, chord, validateField, validateAll };
})();
