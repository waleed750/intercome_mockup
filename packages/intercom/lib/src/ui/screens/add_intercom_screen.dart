import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../config/device_config.dart';
import '../../config/device_identity.dart';
import '../../net/local_ip_address.dart';

final class AddIntercomScreen extends StatefulWidget {
  const AddIntercomScreen({super.key, required this.config, this.onSaved});

  final DeviceConfig config;
  final ValueChanged<DeviceIdentity>? onSaved;

  @override
  State<AddIntercomScreen> createState() => _AddIntercomScreenState();
}

final class _AddIntercomScreenState extends State<AddIntercomScreen> {
  late final TextEditingController _unit =
      TextEditingController(text: widget.config.identity.alias);
  late final TextEditingController _door =
      TextEditingController(text: widget.config.identity.doorName);
  late final TextEditingController _addr =
      TextEditingController(text: widget.config.identity.dstAddr);
  late final TextEditingController _serial =
      TextEditingController(text: widget.config.identity.serial);
  bool _saving = false;
  String? _addrError;
  late Future<String?> _localIp = resolveLocalIpv4Address();

  @override
  void dispose() {
    _unit.dispose();
    _door.dispose();
    _addr.dispose();
    _serial.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Add intercom')),
      body: ListView(
        padding: const EdgeInsets.all(24),
        children: [
          TextField(
              controller: _unit,
              decoration: const InputDecoration(labelText: 'Unit name')),
          const SizedBox(height: 16),
          TextField(
              controller: _door,
              decoration: const InputDecoration(labelText: 'Door name')),
          const SizedBox(height: 16),
          TextField(
            controller: _addr,
            keyboardType: TextInputType.number,
            inputFormatters: [
              FilteringTextInputFormatter.digitsOnly,
              LengthLimitingTextInputFormatter(3),
            ],
            decoration: InputDecoration(
              labelText: 'Address',
              helperText: 'Enter a number from 1 to 999',
              errorText: _addrError,
            ),
            onChanged: (_) {
              if (_addrError != null) setState(() => _addrError = null);
            },
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _serial,
            readOnly: true,
            decoration: const InputDecoration(labelText: 'Serial'),
          ),
          const SizedBox(height: 16),
          FutureBuilder<String?>(
            future: _localIp,
            builder: (context, snapshot) {
              final ip = snapshot.data;
              return ListTile(
                contentPadding: EdgeInsets.zero,
                title: const Text('Current screen IP'),
                subtitle: Text(
                  ip == null || ip.isEmpty ? 'Not connected' : ip,
                ),
                trailing: IconButton(
                  tooltip: 'Refresh IP',
                  icon: const Icon(Icons.refresh),
                  onPressed: () {
                    setState(() => _localIp = resolveLocalIpv4Address());
                  },
                ),
              );
            },
          ),
          const SizedBox(height: 24),
          FilledButton(
            onPressed: _saving ? null : _save,
            child: Text(_saving ? 'Saving...' : 'Save'),
          ),
        ],
      ),
    );
  }

  Future<void> _save() async {
    final address = int.tryParse(_addr.text.trim());
    if (address == null || address < 1 || address > 999) {
      setState(() => _addrError = 'Address must be between 1 and 999');
      return;
    }
    setState(() => _saving = true);
    final identity = DeviceIdentity(
      alias: _unit.text,
      serial: _serial.text,
      dstAddr: address.toString(),
      doorName: _door.text,
    );
    await widget.config.save(identity);
    widget.onSaved?.call(widget.config.identity);
    if (mounted) setState(() => _saving = false);
  }
}
