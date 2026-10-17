(function () {
    'use strict';
    const mounts = new WeakMap();
    // One controller per NPC editor prevents late requests from replacing another NPC's schedules.
    window.chimSchedules = function (root, api, npcId) {
        mounts.get(root)?.abort();
        const controller = new AbortController(); mounts.set(root, controller);
        let clock = {}, rows = [], editId = 0, busy = false, generation = 0;
        root.innerHTML = `<div class="schedule-toolbar"><strong>Schedules</strong><button type="button" data-action="refresh">Refresh</button><button type="button" data-action="new">Add schedule</button></div>
            <p data-clock></p><p data-status role="status" aria-live="polite"></p><div data-list></div>
            <div data-editor hidden class="schedule-card"><h3 data-title>New schedule</h3>
            <label>Activity<input data-field="subject" maxlength="1000" placeholder="Meet at the Bannered Mare"></label>
            <label>Type<select data-field="mode"><option value="visit">Be at the destination</option><option value="stay">Stay for a duration</option><option value="task">Carry out a duty (AI-reported outcome)</option></select></label>
            <label>Find destination<input data-field="search" placeholder="Location name"></label><button type="button" data-action="search">Find locations</button>
            <label>Destination<select data-field="location"><option value="">Select a recognised location</option></select></label>
            <div class="schedule-fields"><label>In-game day number<input data-field="day" type="number" min="0" step="1"></label><label>Appointment time<input data-field="time" type="time" value="19:00"></label>
            <label>Duration (game hours)<input data-field="duration" type="number" min="0" max="8760" step="0.25" value="0"></label><label>Repeat every (game hours)<input data-field="repeat" type="number" min="0" max="8760" step="0.25" value="0"></label></div>
            <p>Use 24 hours for daily appointments, or 0 for one time. Travel starts three game hours early. Late NPCs are teleported to the validated destination.</p>
            <p data-preview></p><button type="button" data-action="save">Save and validate</button><button type="button" data-action="close">Close</button></div>`;
        const one = s => root.querySelector(s), field = n => one(`[data-field="${n}"]`);
        const status = text => { one('[data-status]').textContent = text; };
        const format = ticks => {
            const total = Math.round(Number(ticks) * 24 * 60 / 10000000);
            return `Day ${Math.floor(total / 1440)}, ${String(Math.floor(total / 60) % 24).padStart(2,'0')}:${String(total % 60).padStart(2,'0')}`;
        };
        const appointment = () => {
            const [hour, minute] = field('time').value.split(':').map(Number);
            return Math.round(Number(field('day').value) * 10000000 + (hour * 60 + minute) * 10000000 / 1440);
        };
        async function request(operation, values = {}, write = false) {
            const payload = {npc_id:npcId, operation, epoch:clock.epoch, ...values};
            const response = await fetch(write ? api : `${api}?${new URLSearchParams(payload)}`, {
                method:write?'POST':'GET', cache:'no-store', signal:controller.signal,
                ...(write?{headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)}:{})
            });
            const data = await response.json();
            if (!response.ok || !data.success) throw new Error(data.error || 'Schedule request failed.');
            return data;
        }
        function render(data) {
            clock=data.clock || {}; rows=data.schedules || [];
            one('[data-clock]').textContent = clock.gamets ? `Current game time: ${format(clock.gamets)}` : 'Connect the game to create schedules.';
            const list=one('[data-list]'); list.replaceChildren();
            if (!rows.length) { list.textContent='No schedules for this NPC.'; return; }
            for (const row of rows) {
                const card=document.createElement('article'); card.className='schedule-card';
                const title=document.createElement('strong'); title.textContent=row.subject; card.append(title);
                for(const text of [row.schedule.name, `Appointment: ${format(row.due_gamets)} · Depart: ${format(row.departure_gamets)}`, `Repeat: ${Number(row.repeat_interval_gamets)>0?(Number(row.repeat_interval_gamets)*0.0000024).toFixed(2)+' game hours':'Once'} · ${row.phase || 'pending'}`, ({'validated':'Destination validated in game.','teleported':'Teleported to the appointment.','arrived':'Arrival confirmed.','travelling':'Travel started.','released':'Schedule activity released.','invalid':'The actor or arrival point is unavailable.','stale':'This command belongs to an old game time. Refresh or edit the schedule.','busy':'NPC is busy. The processor will try again.'}[row.result] || row.result)]) {
                    const p=document.createElement('p');p.textContent=text;card.append(p);
                }
                const buttons=document.createElement('div'); buttons.className='schedule-toolbar';
                for(const [action,label] of [['edit','Edit'],['cancel','Cancel'],['retry','Retry'],['delete','Delete']]) {
                    const button=document.createElement('button');button.type='button';button.dataset.action=action;button.dataset.id=row.id;button.textContent=label;buttons.append(button);
                }
                card.append(buttons);list.append(card);
            }
        }
        function open(row) {
            editId=row ? Number(row.id) : 0; one('[data-title]').textContent=row?'Edit schedule':'New schedule';
            field('subject').value=row?.subject || ''; field('mode').value=row?.schedule.mode || 'visit';
            const due=Number(row?.due_gamets || clock.gamets || 0);
            field('day').value=Math.floor(due/10000000); field('time').value=row?format(due).split(', ')[1]:'19:00';
            field('duration').value=row?.schedule.duration_hours || 0;field('repeat').value=Number(row?.repeat_interval_gamets || 0)*0.0000024;
            field('search').value=row?.schedule.name || '';field('location').replaceChildren(new Option('Select a recognised location',''));
            // Keep the saved destination selectable; location_id is its current runtime FormID.
            if(row && Number(row.location_id)>0) { const formid=Number(row.location_id); field('location').append(new Option(`${row.schedule.name || 'Saved destination'} (${formid.toString(16).toUpperCase()})`,String(formid),true,true)); }
            one('[data-editor]').hidden=false; field('subject').focus(); preview();
        }
        function preview() {
            const due=appointment();one('[data-preview]').textContent=Number.isFinite(due)?`Appointment: ${format(due)} · Departure: ${format(Math.max(0,due-1250000))}`:'';
        }
        root.addEventListener('input', preview, {signal:controller.signal});
        // Enter in a schedule input must not submit the surrounding NPC form; buttons, selects and textareas keep their behaviour.
        root.addEventListener('keydown', event => { if(event.key==='Enter' && !event.isComposing && event.target instanceof HTMLInputElement) event.preventDefault(); }, {signal:controller.signal});
        root.addEventListener('click', async event => {
            const button=event.target.closest('[data-action]');if(!button || busy) return;
            const action=button.dataset.action,id=Number(button.dataset.id || 0);
            if(action==='new'||action==='edit') { open(rows.find(r=>Number(r.id)===id));return; }
            if(action==='close') { one('[data-editor]').hidden=true;return; }
            if(['cancel','delete'].includes(action) && !confirm(`${action==='cancel'?'Cancel':'Delete'} this schedule?`)) return;
            const version=++generation; busy=true;root.querySelectorAll('button').forEach(b=>b.disabled=true);status('Working…');
            try {
                if(action==='search') {
                    const data=await request('locations',{search:field('search').value});
                    field('location').replaceChildren(new Option('Select a recognised location',''));
                    data.locations.forEach(l=>field('location').append(new Option(`${l.name} (${Number(l.formid).toString(16).toUpperCase()})`,String(l.formid))));
                    status(data.locations.length?'Choose the exact destination.':'No recognised locations found.');
                } else {
                    const data=action==='save' ? await request('save',{id:editId,subject:field('subject').value,mode:field('mode').value,location_id:field('location').value,due_gamets:appointment(),duration_hours:Number(field('duration').value),repeat_hours:Number(field('repeat').value)},true)
                        : await request(action==='refresh'?'list':action,{id},action!=='refresh');
                    if(version!==generation || controller.signal.aborted)return;
                    render(data);status(action==='save'?'Saved pending game validation. Refresh to see the result.':'Updated.');
                    if(action==='save')one('[data-editor]').hidden=true;
                }
            } catch(error) { if(!controller.signal.aborted)status(error.message); }
            finally { if(!controller.signal.aborted){busy=false;root.querySelectorAll('button').forEach(b=>b.disabled=false);} }
        }, {signal:controller.signal});
        busy=true;
        request('list').then(data=>{if(!controller.signal.aborted)render(data);}).catch(error=>{if(!controller.signal.aborted)status(error.message);}).finally(()=>{busy=false;});
        return () => controller.abort();
    };
})();
