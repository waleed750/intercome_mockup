import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../call/call_controller.dart';
import '../widgets/pulsing_text.dart';
import '../widgets/round_action_button.dart';
import '../widgets/video_surface.dart';

final class IncomingCallScreen extends StatelessWidget {
  const IncomingCallScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final controller = context.watch<CallController>();
    final state = controller.state;
    return Scaffold(
      body: Stack(
        fit: StackFit.expand,
        children: [
          VideoSurface(
            textureId: controller.videoTextureId,
            placeholder: const PulsingText('Calling...'),
          ),
          SafeArea(
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(state.callerLabel,
                      style: Theme.of(context).textTheme.displaySmall),
                  const SizedBox(height: 8),
                  const PulsingText('Incoming call'),
                  const Spacer(),
                  Row(
                    mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                    children: [
                      RoundActionButton(
                        icon: Icons.call_end,
                        caption: 'Decline',
                        backgroundColor: const Color(0xFFD7483D),
                        onPressed: controller.decline,
                      ),
                      RoundActionButton(
                        icon: Icons.call,
                        caption: 'Answer',
                        backgroundColor: const Color(0xFF1DAF8A),
                        onPressed: controller.answer,
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
