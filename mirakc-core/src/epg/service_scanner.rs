use std::sync::Arc;
use std::time::Duration;

use actlet::prelude::*;
use indexmap::IndexMap;
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

pub struct ServiceScanner<T> {
    config: Arc<Config>,
    tuner_manager: T,
}

// TODO: The following implementation has code clones similar to
//       ClockSynchronizer and EitCollector.

impl<T> ServiceScanner<T>
where
    T: Clone,
    T: Call<StartStreaming>,
    T: TriggerFactory<StopStreaming>,
{
    const LABEL: &'static str = "epg.scan-services";

    pub fn new(config: Arc<Config>, tuner_manager: T) -> Self {
        ServiceScanner {
            config,
            tuner_manager,
        }
    }

    pub async fn scan_services<C: Spawn>(
        self,
        ctx: &C,
    ) -> Vec<(EpgChannel, Option<IndexMap<ServiceId, EpgService>>)> {
        let jobs = &self.config.jobs.scan_services;
        let mut results = Vec::new();

        for channel in self.config.channels.iter() {
            // BS4K delivers a TLV stream; use the TLV variant so that the
            // command receives TLV packets.  The arib fork guarantees that
            // scan-services-tlv emits JSON compatible with TsService.
            let command = jobs.command_for(channel.channel_type);
            let result = match Self::scan_services_in_channel(
                channel,
                command,
                jobs.timeout,
                &self.tuner_manager,
                ctx,
            )
            .await
            {
                Ok(services) => {
                    let mut map = IndexMap::new();
                    for service in services.into_iter() {
                        map.insert(service.id, service.clone());
                    }
                    Some(map)
                }
                Err(err) => {
                    tracing::error!(%err, channel.name, "Failed to scan services");
                    None
                }
            };
            results.push((channel.clone().into(), result));
        }

        results
    }

    async fn scan_services_in_channel<C: Spawn>(
        channel: &ChannelConfig,
        command: &str,
        timeout: Duration,
        tuner_manager: &T,
        ctx: &C,
    ) -> anyhow::Result<Vec<EpgService>> {
        tracing::debug!(channel.name, "Scanning services...");

        let user = TunerUser {
            info: TunerUserInfo::Job(Self::LABEL.to_string()),
            priority: (-1).into(),
        };

        let stream = tuner_manager
            .call(StartStreaming {
                channel: channel.clone().into(),
                user,
                stream_id: None,
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

        anyhow::ensure!(!buf.is_empty(), "No service, maybe out of service");

        let services: Vec<TsService> = serde_json::from_slice(&buf)?;
        tracing::debug!(
            channel.name,
            services.len = services.len(),
            "Found services"
        );

        Ok(services
            .into_iter()
            .map(|sv| EpgService::from((channel, &sv)))
            .collect())
    }
}

#[derive(Clone, Deserialize)]
#[cfg_attr(test, derive(Serialize))]
#[serde(rename_all = "camelCase")]
struct TsService {
    nid: Nid,
    #[allow(dead_code)]
    tsid: Tsid,
    sid: Sid,
    #[serde(rename = "type")]
    service_type: u16,
    #[serde(default)]
    logo_id: i16,
    #[serde(default)]
    remote_control_key_id: u16,
    name: String,
}

impl From<(&ChannelConfig, &TsService)> for EpgService {
    fn from((ch, sv): (&ChannelConfig, &TsService)) -> Self {
        EpgService {
            id: ServiceId::new(sv.nid, sv.sid),
            service_type: sv.service_type,
            logo_id: sv.logo_id,
            remote_control_key_id: sv.remote_control_key_id,
            name: sv.name.clone(),
            channel: ch.clone().into(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tuner::stub::TunerManagerStub;
    use assert_matches::assert_matches;
    use test_log::test;

    #[test(tokio::test)]
    async fn test_scan_services_in_channel() {
        let ctx = actlet::stubs::Context::default();

        let stub = TunerManagerStub::default();

        let expected = vec![TsService {
            nid: 1.into(),
            tsid: 2.into(),
            sid: 3.into(),
            service_type: 4,
            logo_id: 0,
            remote_control_key_id: 1,
            name: "service".to_string(),
        }];
        let config_yml = format!(
            r#"
            channels:
              - name: channel
                type: GR
                channel: '0'
            jobs:
              scan-services:
                command: echo '{}'
        "#,
            serde_json::to_string(&expected).unwrap()
        );
        let config = Arc::new(serde_norway::from_str::<Config>(&config_yml).unwrap());
        let result = ServiceScanner::scan_services_in_channel(
            &config.channels[0],
            &config.jobs.scan_services.command,
            config.jobs.scan_services.timeout,
            &stub,
            &ctx,
        )
        .await;
        assert_matches!(result, Ok(services) => {
            assert_eq!(services.len(), 1);
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
              scan-services:
                command: false
        "#,
            )
            .unwrap(),
        );
        let result = ServiceScanner::scan_services_in_channel(
            &config.channels[0],
            &config.jobs.scan_services.command,
            config.jobs.scan_services.timeout,
            &stub,
            &ctx,
        )
        .await;
        assert_matches!(result, Err(err) => {
            assert_eq!(format!("{err}"), "No service, maybe out of service");
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
              scan-services:
                command: sleep 10
                timeout: 10ms
        "#,
            )
            .unwrap(),
        );
        let result = ServiceScanner::scan_services_in_channel(
            &config.channels[0],
            &config.jobs.scan_services.command,
            config.jobs.scan_services.timeout,
            &stub,
            &ctx,
        )
        .await;
        assert_matches!(result, Err(err) => {
            assert!(err.is::<tokio::time::error::Elapsed>());
        });
    }

    #[test(tokio::test)]
    async fn test_scan_services_selects_command_per_channel_type() {
        use crate::config::ScanServicesJobConfig;

        let config = serde_norway::from_str::<Config>(
            r#"
            channels:
              - name: gr
                type: GR
                channel: '0'
              - name: bs4k
                type: BS4K
                channel: '45168'
            jobs:
              scan-services:
                command: echo GR
                command-bs4k: echo BS4K
        "#,
        )
        .unwrap();
        assert_eq!(
            config.jobs.scan_services.command_for(ChannelType::GR),
            "echo GR"
        );
        assert_eq!(
            config.jobs.scan_services.command_for(ChannelType::BS4K),
            "echo BS4K"
        );
        // 2K default is unchanged, BS4K default uses the fork binary.
        assert!(!ScanServicesJobConfig::default().command.contains("-tlv"));
        assert!(
            ScanServicesJobConfig::default()
                .command_for(ChannelType::BS4K)
                .contains("mirakc-arib-tlv scan-services-tlv")
        );
    }

    #[test(tokio::test)]
    async fn test_scan_services_in_channel_bs4k() {
        use crate::config::ScanServicesJobConfig;

        let ctx = actlet::stubs::Context::default();

        let stub = TunerManagerStub::default();

        let expected = vec![TsService {
            nid: 4.into(),
            tsid: 5.into(),
            sid: 6.into(),
            service_type: 1,
            logo_id: -1,
            remote_control_key_id: 0,
            name: "bs4k-service".to_string(),
        }];
        // The 2K command fails while the BS4K command succeeds, proving
        // that the BS4K channel uses command-bs4k.
        let config_yml = format!(
            r#"
            channels:
              - name: bs4k
                type: BS4K
                channel: '45168'
            jobs:
              scan-services:
                command: false
                command-bs4k: echo '{}'
        "#,
            serde_json::to_string(&expected).unwrap()
        );
        let config = Arc::new(serde_norway::from_str::<Config>(&config_yml).unwrap());
        let channel = &config.channels[0];
        assert_eq!(channel.channel_type, ChannelType::BS4K);
        let result = ServiceScanner::scan_services_in_channel(
            channel,
            config.jobs.scan_services.command_for(channel.channel_type),
            config.jobs.scan_services.timeout,
            &stub,
            &ctx,
        )
        .await;
        assert_matches!(result, Ok(services) => {
            assert_eq!(services.len(), 1);
            assert_eq!(services[0].name, "bs4k-service");
            assert_eq!(services[0].id, ServiceId::new(4.into(), 6.into()));
        });

        // And a GR channel keeps using the 2K command.
        let config_yml = format!(
            r#"
            channels:
              - name: gr
                type: GR
                channel: '0'
            jobs:
              scan-services:
                command: echo '{}'
                command-bs4k: false
        "#,
            serde_json::to_string(&expected).unwrap()
        );
        let config = Arc::new(serde_norway::from_str::<Config>(&config_yml).unwrap());
        let channel = &config.channels[0];
        assert_eq!(channel.channel_type, ChannelType::GR);
        let result = ServiceScanner::scan_services_in_channel(
            channel,
            config.jobs.scan_services.command_for(channel.channel_type),
            config.jobs.scan_services.timeout,
            &stub,
            &ctx,
        )
        .await;
        assert_matches!(result, Ok(services) => {
            assert_eq!(services.len(), 1);
        });

        // Default BS4K template uses the TLV fork binary.
        assert!(
            ScanServicesJobConfig::default()
                .command_for(ChannelType::BS4K)
                .contains("mirakc-arib-tlv scan-services-tlv")
        );
    }
}
