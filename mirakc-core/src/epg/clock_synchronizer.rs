use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use actlet::prelude::*;
use serde::Deserialize;
use tokio::io::AsyncReadExt;

#[cfg(test)]
use serde::Serialize;
use tracing::Instrument;

use crate::command_util;
use crate::config::ChannelConfig;
use crate::config::Config;
use crate::epg::*;
use crate::models::*;
use crate::tuner::*;

pub struct ClockSynchronizer<T> {
    config: Arc<Config>,
    tuner_manager: T,
}

// TODO: The following implementation has code clones similar to
//       EitCollector and ServiceScanner.

impl<T> ClockSynchronizer<T>
where
    T: Clone,
    T: Call<StartStreaming>,
    T: TriggerFactory<StopStreaming>,
{
    const LABEL: &'static str = "epg.sync-clocks";

    pub fn new(config: Arc<Config>, tuner_manager: T) -> Self {
        ClockSynchronizer {
            config,
            tuner_manager,
        }
    }

    pub async fn sync_clocks<C: Spawn>(
        self,
        ctx: &C,
    ) -> Vec<(EpgChannel, Option<HashMap<ServiceId, Clock>>)> {
        let command = &self.config.jobs.sync_clocks.command;
        let mut results = Vec::new();

        for channel in self.config.channels.iter() {
            // BS4K has no PCR clock concept, so clock synchronization is
            // disabled for BS4K channels.  Push None so that previously
            // synchronized clocks (if any) are reused by update_clocks().
            if channel.channel_type == ChannelType::BS4K {
                tracing::debug!(channel.name, "Skipping clock sync for BS4K");
                results.push((channel.clone().into(), None));
                continue;
            }
            let result = match Self::sync_clocks_in_channel(
                channel,
                command,
                self.config.jobs.sync_clocks.timeout,
                &self.tuner_manager,
                ctx,
            )
            .await
            {
                Ok(clocks) => {
                    let mut map = HashMap::new();
                    for clock in clocks.into_iter() {
                        let service_id = ServiceId::new(clock.nid, clock.sid);
                        map.insert(service_id, clock.clock.clone());
                    }
                    Some(map)
                }
                Err(err) => {
                    tracing::error!(%err, channel.name, "Failed to synchronize clocks");
                    None
                }
            };
            results.push((channel.clone().into(), result));
        }

        results
    }

    async fn sync_clocks_in_channel<C: Spawn>(
        channel: &ChannelConfig,
        command: &str,
        timeout: Duration,
        tuner_manager: &T,
        ctx: &C,
    ) -> anyhow::Result<Vec<SyncClock>> {
        tracing::debug!(channel.name, "Synchronizing clocks...");

        let user = TunerUser {
            info: TunerUserInfo::Job(Self::LABEL.to_string()),
            priority: (-1).into(),
        };

        let stream = tuner_manager
            .call(StartStreaming {
                channel: channel.clone().into(),
                user,
                stream_id: None,
                tuner: None,
            })
            .await??;

        let msg = StopStreaming { id: stream.id() };
        let stop_trigger = tuner_manager.trigger(msg);

        let template = mustache::compile_str(command)?;
        let data = mustache::MapBuilder::new()
            .insert("sids", &channel.services)?
            .insert("xsids", &channel.excluded_services)?
            .build();
        let cmd = template.render_data_to_string(&data)?;

        let mut pipeline = command_util::spawn_pipeline(vec![cmd], stream.id(), Self::LABEL, ctx)?;

        let (input, mut output) = pipeline.take_endpoints();

        let (handle, _) = ctx.spawn_task(stream.pipe(input).in_current_span());

        let mut buf = Vec::new();
        tokio::time::timeout(timeout, output.read_to_end(&mut buf)).await??;

        drop(stop_trigger);

        // Explicitly dropping the output of the pipeline is needed.  The output
        // holds the child processes and it kills them when dropped.
        drop(pipeline);

        // Wait for the task so that the tuner is released before a request for
        // streaming in the next iteration.
        let _ = handle.await;

        anyhow::ensure!(!buf.is_empty(), "No clock, maybe out of service");

        let clocks: Vec<SyncClock> = serde_json::from_slice(&buf)?;
        tracing::debug!(
            channel.name,
            clocks.len = clocks.len(),
            "Synchronized clocks"
        );

        Ok(clocks)
    }
}

#[derive(Clone, Deserialize)]
#[cfg_attr(test, derive(Debug, Serialize))]
#[serde(rename_all = "camelCase")]
pub struct SyncClock {
    pub nid: Nid,
    #[allow(dead_code)]
    pub tsid: Tsid,
    pub sid: Sid,
    pub clock: Clock,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tuner::stub::TunerManagerStub;
    use assert_matches::assert_matches;
    use test_log::test;

    #[test(tokio::test)]
    async fn test_sync_clocks_in_channel() {
        let ctx = actlet::stubs::Context::default();

        let stub = TunerManagerStub::default();

        let expected = vec![SyncClock {
            nid: 1.into(),
            tsid: 2.into(),
            sid: 3.into(),
            clock: Clock {
                pid: 1,
                pcr: 2,
                time: 3,
            },
        }];
        let config_yml = format!(
            r#"
            channels:
              - name: channel
                type: GR
                channel: '0'
            jobs:
              sync-clocks:
                command: echo '{}'
        "#,
            serde_json::to_string(&expected).unwrap()
        );
        let config = Arc::new(serde_norway::from_str::<Config>(&config_yml).unwrap());
        let result = ClockSynchronizer::sync_clocks_in_channel(
            &config.channels[0],
            &config.jobs.sync_clocks.command,
            config.jobs.sync_clocks.timeout,
            &stub,
            &ctx,
        )
        .await;
        assert_matches!(result, Ok(clocks) => {
            assert_eq!(clocks.len(), 1);
        });

        // Emulate out of services by using `false`
        let config = Arc::new(
            serde_norway::from_str::<Config>(
                r#"
            channels:
              - name: channel
                type: GR
                channel: '0'
            jobs:
              sync-clocks:
                command: false
        "#,
            )
            .unwrap(),
        );
        let result = ClockSynchronizer::sync_clocks_in_channel(
            &config.channels[0],
            &config.jobs.sync_clocks.command,
            config.jobs.sync_clocks.timeout,
            &stub,
            &ctx,
        )
        .await;
        assert_matches!(result, Err(err) => {
            assert_eq!(format!("{err}"), "No clock, maybe out of service");
        });

        // Timed out
        let config = Arc::new(
            serde_norway::from_str::<Config>(
                r#"
            channels:
              - name: channel
                type: GR
                channel: '0'
            jobs:
              sync-clocks:
                command: sleep 10
                timeout: 10ms
        "#,
            )
            .unwrap(),
        );
        let result = ClockSynchronizer::sync_clocks_in_channel(
            &config.channels[0],
            &config.jobs.sync_clocks.command,
            config.jobs.sync_clocks.timeout,
            &stub,
            &ctx,
        )
        .await;
        assert_matches!(result, Err(err) => {
            assert!(err.is::<tokio::time::error::Elapsed>());
        });
    }

    #[test(tokio::test)]
    async fn test_sync_clocks_skips_bs4k_channels() {
        let ctx = actlet::stubs::Context::default();

        let stub = TunerManagerStub::default();

        let expected = vec![SyncClock {
            nid: 1.into(),
            tsid: 2.into(),
            sid: 3.into(),
            clock: Clock {
                pid: 1,
                pcr: 2,
                time: 3,
            },
        }];
        // The command succeeds, so a GR channel yields clocks while a BS4K
        // channel must still be skipped (None).
        let config_yml = format!(
            r#"
            channels:
              - name: gr
                type: GR
                channel: '0'
              - name: bs4k
                type: BS4K
                channel: '45168'
            jobs:
              sync-clocks:
                command: echo '{}'
        "#,
            serde_json::to_string(&expected).unwrap()
        );
        let config = Arc::new(serde_norway::from_str::<Config>(&config_yml).unwrap());
        let sync = ClockSynchronizer::new(config, stub);
        let results = sync.sync_clocks(&ctx).await;
        assert_eq!(results.len(), 2);
        assert_eq!(results[0].0.channel_type, ChannelType::GR);
        assert_matches!(&results[0].1, Some(clocks) => {
            assert_eq!(clocks.len(), 1);
        });
        assert_eq!(results[1].0.channel_type, ChannelType::BS4K);
        assert_matches!(&results[1].1, None);
    }
}
