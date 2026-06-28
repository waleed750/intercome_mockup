import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../call/call_controller.dart';
import 'add_intercom_screen.dart';
import '../widgets/status_pill.dart';

final class IdleScreen extends StatelessWidget {
  const IdleScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final controller = context.watch<CallController>();
    final state = controller.state;
    return Scaffold(
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Expanded(
                    child: Text(
                      state.unitName.isEmpty
                          ? 'Indoor Station'
                          : state.unitName,
                      style: Theme.of(context).textTheme.headlineMedium,
                    ),
                  ),
                  StatusPill(
                      label: state.onWifi ? 'Wi-Fi' : 'Offline',
                      active: state.onWifi),
                  const SizedBox(width: 8),
                  IconButton(
                    tooltip: 'Settings',
                    icon: const Icon(Icons.settings),
                    onPressed: () {
                      Navigator.of(context).push(
                        MaterialPageRoute<void>(
                          builder: (_) => AddIntercomScreen(
                            config: controller.deviceConfig,
                            onSaved: (_) => controller.refreshIdentity(),
                          ),
                        ),
                      );
                    },
                  ),
                ],
              ),
              const Spacer(),
              Text('Paired door',
                  style: Theme.of(context).textTheme.labelLarge),
              const SizedBox(height: 8),
              Text(state.pairedDoor,
                  style: Theme.of(context).textTheme.displaySmall),
              const SizedBox(height: 24),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  StatusPill(
                    label: state.discoveryListening ? 'UDP 8089' : 'UDP off',
                    active: state.discoveryListening,
                  ),
                  StatusPill(
                    label: state.tcpServerListening ? 'TCP 8189' : 'TCP off',
                    active: state.tcpServerListening,
                  ),
                ],
              ),
              const SizedBox(height: 24),
              OutlinedButton.icon(
                onPressed: controller.simulateIncomingCall,
                icon: const Icon(Icons.call_received),
                label: const Text('Simulate call'),
              ),
              const Spacer(),
              if (state.transientMessage != null)
                Align(
                  alignment: Alignment.center,
                  child: Text(state.transientMessage!,
                      style: Theme.of(context).textTheme.titleMedium),
                ),
            ],
          ),
        ),
      ),
    );
  }
}
