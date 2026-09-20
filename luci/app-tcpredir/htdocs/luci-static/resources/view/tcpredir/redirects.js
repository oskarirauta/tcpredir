'use strict';
'require view';
'require form';
'require ui';
'require uci';
'require rpc';

/* The configuration file is the daemon's steady state, so this is the view that
   matters: everything here survives a restart. Runtime redirects added over ubus
   are shown on the Status page instead, where it is clear that they are
   temporary. */

var callReload = rpc.declare({
	object: 'tcpredir',
	method: 'reload'
});

return view.extend({
	load: function() {
		return uci.load('tcpredir');
	},

	render: function() {
		var m, s, o;

		m = new form.Map('tcpredir', _('Port Redirects'),
			_('tcpredir forwards a port on this router to an address it can reach - a host on another VLAN, a container on an isolated network, a machine behind a VPN. It proxies in userspace, so the target sees connections coming from the router rather than from the original client.'));

		s = m.section(form.GridSection, 'redirect', _('Redirects'));
		s.addremove = true;
		s.anonymous = false;
		s.sortable  = true;
		s.nodescriptions = true;

		o = s.option(form.Flag, 'enabled', _('Enabled'));
		o.default  = '1';
		o.editable = true;

		o = s.option(form.ListValue, 'proto', _('Protocol'));
		o.value('tcp',  _('TCP'));
		o.value('udp',  _('UDP'));
		o.value('both', _('TCP + UDP'));
		o.default = 'tcp';

		o = s.option(form.Value, 'listen_ip', _('Listen address'),
			_('Which of the router\'s addresses to accept connections on. Leave at 0.0.0.0 for all of them, or set a LAN address to keep the redirect off the WAN.'));
		o.datatype   = 'ipaddr';
		o.placeholder = '0.0.0.0';
		o.default    = '0.0.0.0';

		o = s.option(form.Value, 'listen_port', _('Listen port'));
		o.datatype = 'port';
		o.rmempty  = false;

		o = s.option(form.Value, 'target_ip', _('Target address'));
		o.datatype = 'ipaddr';
		o.rmempty  = false;

		o = s.option(form.Value, 'target_port', _('Target port'));
		o.datatype = 'port';
		o.rmempty  = false;

		/* Tuning lives behind the grid's edit dialog: the defaults are right for
		   almost everything, and putting them in the table would bury the four
		   fields that actually describe the redirect. */
		o = s.option(form.Value, 'connect_timeout', _('Connect timeout'),
			_('Seconds to wait for the target to accept. Keep it short: a client waits this long before it learns the target is down.'));
		o.datatype    = 'uinteger';
		o.placeholder = '10';
		o.modalonly   = true;

		o = s.option(form.Value, 'idle_timeout', _('Idle timeout'),
			_('Seconds a TCP connection may sit with no traffic before it is closed. Raise it for long-lived sessions such as SSH or a database connection pool.'));
		o.datatype    = 'uinteger';
		o.placeholder = '300';
		o.modalonly   = true;

		o = s.option(form.Value, 'udp_timeout', _('UDP session timeout'),
			_('Seconds of silence before a UDP flow is forgotten. UDP has no close, so this is the only thing that reclaims the socket.'));
		o.datatype    = 'uinteger';
		o.placeholder = '5';
		o.modalonly   = true;
		o.depends('proto', 'udp');
		o.depends('proto', 'both');

		o = s.option(form.Value, 'max_connections', _('Maximum connections'),
			_('Connections accepted at once for this redirect. Further clients wait rather than being refused.'));
		o.datatype    = 'uinteger';
		o.placeholder = '128';
		o.modalonly   = true;

		return m.render();
	},

	/* Apply the file by asking the daemon to re-read it, rather than restarting
	   the service. A reload replaces only what came from this file and leaves
	   runtime redirects - a container's published port, say - connected. */
	handleSaveApply: function(ev, mode) {
		var self = this;
		return this.super('handleSaveApply', [ev, mode]).then(function() {
			return callReload().then(function() {
				ui.addNotification(null,
					E('p', _('Redirects reloaded. Redirects added at runtime were left running.')), 'info');
			}, function(err) {
				ui.addNotification(null,
					E('p', _('Saved, but the daemon could not be reloaded: %s').format(err.message || err)), 'warning');
			});
		});
	}
});
