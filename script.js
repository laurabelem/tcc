    async function obterEstado() {
  const resp = await fetch('/api/state');
  const data = await resp.json();

  const temp = data.temp;
  const umid = data.umid;
  const alvo = data.alvo;
  const lamp = data.lampada === 1;

  document.getElementById('tempAtual').textContent =
    isNaN(temp) ? '-- °C' : temp.toFixed(1) + ' °C';

  document.getElementById('umidAtual').textContent =
    isNaN(umid) ? '-- %' : umid.toFixed(1) + ' %';

  document.getElementById('setpointAtual').textContent =
    alvo.toFixed(1) + ' °C';

  document.getElementById('valorSetpoint').value = alvo.toFixed(1);

  document.getElementById('estadoLampada').textContent = lamp ? 'Ligada' : 'Desligada';
}

async function obterHoje() {
  const resp = await fetch('/api/today');
  const lista = await resp.json();
  atualizarTabela(lista);
  atualizarGrafico(lista);
}

function atualizarTabela(lista) {
  const tbody = document.querySelector('#tabela tbody');
  tbody.innerHTML = '';

  lista.forEach(reg => {
    const tr = document.createElement('tr');
    const tdHora = document.createElement('td');
    const tdTemp = document.createElement('td');
    const tdUmid = document.createElement('td');

    tdHora.textContent = reg.hora;
    tdTemp.textContent = reg.temp.toFixed(1);
    tdUmid.textContent = reg.umid.toFixed(1);

    tr.appendChild(tdHora);
    tr.appendChild(tdTemp);
    tr.appendChild(tdUmid);
    tbody.appendChild(tr);
  });
}

function atualizarGrafico(lista) {
  if (!lista.length) {
    document.getElementById('linhaTemp').setAttribute('points', '');
    document.getElementById('linhaUmid').setAttribute('points', '');
    return;
  }

  let minTemp = lista[0].temp;
  let maxTemp = lista[0].temp;
  let minUmid = lista[0].umid;
  let maxUmid = lista[0].umid;

  lista.forEach(r => {
    if (r.temp < minTemp) minTemp = r.temp;
    if (r.temp > maxTemp) maxTemp = r.temp;
    if (r.umid < minUmid) minUmid = r.umid;
    if (r.umid > maxUmid) maxUmid = r.umid;
  });

  if (Math.abs(maxTemp - minTemp) < 1) {
    maxTemp += 0.5;
    minTemp -= 0.5;
  }
  if (Math.abs(maxUmid - minUmid) < 1) {
    maxUmid += 0.5;
    minUmid -= 0.5;
  }

  const totalMin = 1439;
  const pontosTemp = [];
  const pontosUmid = [];

  lista.forEach(r => {
    const [hStr, mStr] = r.hora.split(':');
    const h = parseInt(hStr, 10);
    const m = parseInt(mStr, 10);
    const idx = h * 60 + m;

    const x = 5 + (idx / totalMin) * 90;

    const tNorm = (r.temp - minTemp) / (maxTemp - minTemp);
    const uNorm = (r.umid - minUmid) / (maxUmid - minUmid);

    const yTemp = 55 - tNorm * 50;
    const yUmid = 55 - uNorm * 50;

    pontosTemp.push(x.toFixed(2) + ',' + yTemp.toFixed(2));
    pontosUmid.push(x.toFixed(2) + ',' + yUmid.toFixed(2));
  });

  document.getElementById('linhaTemp').setAttribute('points', pontosTemp.join(' '));
  document.getElementById('linhaUmid').setAttribute('points', pontosUmid.join(' '));
}

async function enviarSetpoint(e) {
  e.preventDefault();
  const valor = document.getElementById('valorSetpoint').value;
  const formData = new FormData();
  formData.append('valor', valor);

  await fetch('/api/setpoint', {
    method: 'POST',
    body: formData
  });

  await obterEstado();
}

document.getElementById('formSetpoint').addEventListener('submit', enviarSetpoint);

async function atualizarTudo() {
  await obterEstado();
  await obterHoje();
}

atualizarTudo();
setInterval(atualizarTudo, 60000);
