import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../call/call_controller.dart';
import '../widgets/round_action_button.dart';
import '../widgets/video_surface.dart';

final class InCallScreen extends StatelessWidget {
  const InCallScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final controller = context.watch<CallController>();
    final state = controller.state;
    return Scaffold(
      body: Stack(
        fit: StackFit.expand,
        children: [
          VideoSurface(textureId: controller.videoTextureId),
          SafeArea(
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(state.callerLabel,
                      style: Theme.of(context).textTheme.headlineMedium),
                  if (!state.micAvailable)
                    Padding(
                      padding: const EdgeInsets.only(top: 8),
                      child: Text('Microphone unavailable',
                          style: TextStyle(
                              color: Theme.of(context).colorScheme.error)),
                    ),
                  const Spacer(),
                  if (state.transientMessage != null)
                    Center(
                        child: Text(state.transientMessage!,
                            style: Theme.of(context).textTheme.titleLarge)),
                  const SizedBox(height: 16),
                  Row(
                    mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                    children: [
                      RoundActionButton(
                        icon: state.muted ? Icons.mic_off : Icons.mic,
                        caption: state.muted ? 'Muted' : 'Mute',
                        onPressed: () => controller.setMuted(!state.muted),
                      ),
                      RoundActionButton(
                        icon: Icons.lock_open,
                        caption: 'Unlock',
                        backgroundColor: const Color(0xFF1DAF8A),
                        onPressed: controller.unlock,
                      ),
                      RoundActionButton(
                        icon: Icons.call_end,
                        caption: 'End',
                        backgroundColor: const Color(0xFFD7483D),
                        onPressed: controller.endCall,
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}
