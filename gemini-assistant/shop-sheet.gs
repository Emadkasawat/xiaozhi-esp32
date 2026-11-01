/**
 * سكريبت شيت المحل: أسعار الأصناف ودفتر الآجل لجهاز Gemini.
 * 1) غيّر كلمة السر اللي تحت لكلمة سر من عندك (حروف وأرقام إنجليزي من غير مسافات).
 * 2) Deploy > New deployment > Web app، Execute as: Me، Who has access: Anyone.
 * 3) انسخ رابط الـ Web app وحطه في صفحة إعداد الجهاز مع نفس كلمة السر.
 */
const PASSWORD = 'CHANGE-ME';

const ITEMS = 'الأصناف';
const CREDIT = 'الآجل';

function doGet(e) {
  const p = e.parameter || {};
  if (PASSWORD === 'CHANGE-ME' || p.token !== PASSWORD) return out('كلمة سر الشيت غلط');
  try {
    switch (p.action) {
      case 'get_price': return out(getPrice(p.item));
      case 'set_price': return out(setPrice(p.item, p.price, p.note));
      case 'add_credit': return out(addCredit(p.customer, p.amount, p.kind, p.note));
      case 'balance': return out(balance(p.customer));
      case 'debtors': return out(debtors());
      case 'ping': return out('الشيت متوصل');
    }
    return out('أمر مش معروف');
  } catch (err) {
    return out('حصل خطأ في الشيت: ' + err.message);
  }
}

function out(text) {
  return ContentService.createTextOutput(text);
}

function getSheet(name, headers) {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  let sh = ss.getSheetByName(name);
  if (!sh) {
    sh = ss.insertSheet(name);
    sh.appendRow(headers);
    sh.setFrozenRows(1);
    sh.setRightToLeft(true);
    sh.getRange(1, 1, 1, headers.length).setFontWeight('bold');
  }
  return sh;
}

function itemsSheet() { return getSheet(ITEMS, ['الصنف', 'السعر', 'ملاحظة', 'آخر تعديل']); }
function creditSheet() { return getSheet(CREDIT, ['التاريخ', 'الزبون', 'أخد', 'دفع', 'ملاحظة']); }

// Makes Arabic spelling differences (أ/ا، ة/ه، ى/ي، تشكيل) not matter.
function norm(s) {
  return String(s || '').trim().toLowerCase()
    .replace(/[ً-ْـ]/g, '')
    .replace(/[أإآ]/g, 'ا')
    .replace(/ة/g, 'ه')
    .replace(/ى/g, 'ي')
    .replace(/\s+/g, ' ');
}

function num(v) {
  const n = Number(String(v || '').replace(/[٠-٩]/g, d => '٠١٢٣٤٥٦٧٨٩'.indexOf(d)).replace(/,/g, '.'));
  return isNaN(n) ? null : n;
}

function rows(sh) {
  const last = sh.getLastRow();
  if (last < 2) return [];
  return sh.getRange(2, 1, last - 1, sh.getLastColumn()).getValues()
    .map((r, i) => ({ row: i + 2, v: r }));
}

// Exact match first, then partial match in either direction.
function findByName(list, col, query) {
  const q = norm(query);
  if (!q) return [];
  const exact = list.filter(r => norm(r.v[col]) === q);
  if (exact.length) return exact;
  return list.filter(r => {
    const n = norm(r.v[col]);
    return n && (n.indexOf(q) >= 0 || q.indexOf(n) >= 0);
  });
}

function getPrice(item) {
  const found = findByName(rows(itemsSheet()), 0, item);
  if (!found.length) return 'الصنف "' + item + '" مش موجود في الشيت';
  return found.slice(0, 6).map(r =>
    r.v[0] + ': ' + r.v[1] + ' جنيه' + (r.v[2] ? ' (' + r.v[2] + ')' : '')).join('، ');
}

function setPrice(item, price, note) {
  const p = num(price);
  if (!item || p === null) return 'محتاج اسم الصنف والسعر';
  const sh = itemsSheet();
  const exact = rows(sh).filter(r => norm(r.v[0]) === norm(item));
  const now = new Date();
  if (exact.length) {
    const r = exact[0].row;
    sh.getRange(r, 2).setValue(p);
    if (note) sh.getRange(r, 3).setValue(note);
    sh.getRange(r, 4).setValue(now);
    return 'تمام، ' + exact[0].v[0] + ' بقى بـ ' + p + ' جنيه';
  }
  sh.appendRow([item, p, note || '', now]);
  return 'ضفت صنف جديد: ' + item + ' بـ ' + p + ' جنيه';
}

// Uses the spelling already in the ledger when the customer is known.
function customerName(list, customer) {
  const found = findByName(list, 1, customer);
  const names = [...new Set(found.map(r => String(r.v[1])))];
  return names.length === 1 ? names[0] : customer;
}

function addCredit(customer, amount, kind, note) {
  const a = num(amount);
  if (!customer || a === null || a <= 0) return 'محتاج اسم الزبون والمبلغ';
  const sh = creditSheet();
  const name = customerName(rows(sh), customer);
  const paid = kind === 'paid';
  sh.appendRow([new Date(), name, paid ? '' : a, paid ? a : '', note || '']);
  const left = totalFor(rows(sh), name);
  return (paid ? name + ' دفع ' + a : 'اتسجل على ' + name + ' ' + a) +
    '. الباقي عليه دلوقتي ' + left + ' جنيه';
}

function totalFor(list, name) {
  return list.filter(r => norm(r.v[1]) === norm(name))
    .reduce((s, r) => s + (num(r.v[2]) || 0) - (num(r.v[3]) || 0), 0);
}

function balance(customer) {
  const list = rows(creditSheet());
  const found = findByName(list, 1, customer);
  const names = [...new Set(found.map(r => String(r.v[1])))];
  if (!names.length) return 'مفيش حد اسمه "' + customer + '" في دفتر الآجل';
  return names.slice(0, 5).map(n => n + ' عليه ' + totalFor(list, n) + ' جنيه').join('، ');
}

function debtors() {
  const list = rows(creditSheet());
  const totals = {};
  const display = {};
  list.forEach(r => {
    const k = norm(r.v[1]);
    if (!k) return;
    display[k] = display[k] || String(r.v[1]);
    totals[k] = (totals[k] || 0) + (num(r.v[2]) || 0) - (num(r.v[3]) || 0);
  });
  const owing = Object.keys(totals).filter(k => totals[k] > 0).sort((a, b) => totals[b] - totals[a]);
  if (!owing.length) return 'محدش عليه فلوس';
  const sum = owing.reduce((s, k) => s + totals[k], 0);
  return owing.slice(0, 15).map(k => display[k] + ' ' + totals[k]).join('، ') +
    '. الإجمالي ' + sum + ' جنيه';
}
